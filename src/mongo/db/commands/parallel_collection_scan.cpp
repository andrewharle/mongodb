
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/multi_iterator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using stdx::make_unique;

namespace {

class ParallelCollectionScanCmd : public BasicCommand {
public:
    ParallelCollectionScanCmd() : BasicCommand("parallelCollectionScan") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const override {
        return level != repl::ReadConcernLevel::kSnapshotReadConcern;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kCommand;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

        if (!authSession->isAuthorizedToParseNamespaceElement(cmdObj.firstElement())) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        const auto hasTerm = false;
        return authSession->checkAuthForFind(
            AutoGetCollection::resolveNamespaceStringOrUUID(
                opCtx, CommandHelpers::parseNsOrUUID(dbname, cmdObj)),
            hasTerm);
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        AutoGetCollectionForReadCommand ctx(opCtx, CommandHelpers::parseNsOrUUID(dbname, cmdObj));
        const auto nss = ctx.getNss();

        Collection* const collection = ctx.getCollection();
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "ns does not exist: " << nss.ns(),
                collection);

        size_t numCursors = static_cast<size_t>(cmdObj["numCursors"].numberInt());
        uassert(ErrorCodes::BadValue,
                str::stream() << "numCursors has to be between 1 and 10000"
                              << " was: "
                              << numCursors,
                numCursors >= 1 && numCursors <= 10000);

        std::vector<std::unique_ptr<RecordCursor>> iterators;
        // Opening multiple cursors on a capped collection and reading them in parallel can produce
        // behavior that is not well defined. This can be removed when support for parallel
        // collection scan on capped collections is officially added. The 'getCursor' function
        // ensures that the cursor returned iterates the capped collection in proper document
        // insertion order.
        if (collection->isCapped()) {
            iterators.push_back(collection->getCursor(opCtx));
            numCursors = 1;
        } else {
            iterators = collection->getManyCursors(opCtx);
            if (iterators.size() < numCursors) {
                numCursors = iterators.size();
            }
        }

        std::vector<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> execs;
        for (size_t i = 0; i < numCursors; i++) {
            unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
            unique_ptr<MultiIteratorStage> mis =
                make_unique<MultiIteratorStage>(opCtx, ws.get(), collection);

            // Takes ownership of 'ws' and 'mis'.
            const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
            auto statusWithPlanExecutor = PlanExecutor::make(
                opCtx,
                std::move(ws),
                std::move(mis),
                collection,
                readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern
                    ? PlanExecutor::INTERRUPT_ONLY
                    : PlanExecutor::YIELD_AUTO);
            invariant(statusWithPlanExecutor.isOK());
            execs.push_back(std::move(statusWithPlanExecutor.getValue()));
        }

        // transfer iterators to executors using a round-robin distribution.
        // TODO consider using a common work queue once invalidation issues go away.
        for (size_t i = 0; i < iterators.size(); i++) {
            auto& planExec = execs[i % execs.size()];
            MultiIteratorStage* mis = checked_cast<MultiIteratorStage*>(planExec->getRootStage());
            mis->addIterator(std::move(iterators[i]));
        }

        BSONArrayBuilder bucketsBuilder;
        for (auto&& exec : execs) {
            // Need to save state while yielding locks between now and getMore().
            exec->saveState();
            exec->detachFromOperationContext();

            // Create and register a new ClientCursor.
            auto pinnedCursor = collection->getCursorManager()->registerCursor(
                opCtx,
                {std::move(exec),
                 nss,
                 AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
                 repl::ReadConcernArgs::get(opCtx).getLevel(),
                 cmdObj});
            pinnedCursor.getCursor()->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());

            BSONObjBuilder threadResult;
            appendCursorResponseObject(
                pinnedCursor.getCursor()->cursorid(), nss.ns(), BSONArray(), &threadResult);
            threadResult.appendBool("ok", 1);

            bucketsBuilder.append(threadResult.obj());
        }
        result.appendArray("cursors", bucketsBuilder.obj());

        return true;
    }

} parallelCollectionScanCmd;

}  // namespace
}  // namespace mongo
