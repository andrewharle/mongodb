// distinct.cpp


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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/parsed_distinct.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

namespace dps = dotted_path_support;

class DistinctCommand : public BasicCommand {
public:
    DistinctCommand() : BasicCommand("distinct") {}

    std::string help() const override {
        return "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const override {
        return true;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
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

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   BSONObjBuilder* out) const override {
        std::string dbname = request.getDatabase().toString();
        const BSONObj& cmdObj = request.body;
        // Acquire locks. The RAII object is optional, because in the case of a view, the locks
        // need to be released.
        boost::optional<AutoGetCollectionForReadCommand> ctx;
        ctx.emplace(opCtx,
                    CommandHelpers::parseNsCollectionRequired(dbname, cmdObj),
                    AutoGetCollection::ViewMode::kViewsPermitted);
        const auto nss = ctx->getNss();

        const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
        auto parsedDistinct =
            uassertStatusOK(ParsedDistinct::parse(opCtx, nss, cmdObj, extensionsCallback, true));

        if (ctx->getView()) {
            // Relinquish locks. The aggregation command will re-acquire them.
            ctx.reset();

            auto viewAggregation = parsedDistinct.asAggregationCommand();
            if (!viewAggregation.isOK()) {
                return viewAggregation.getStatus();
            }

            auto viewAggRequest =
                AggregationRequest::parseFromBSON(nss, viewAggregation.getValue(), verbosity);
            if (!viewAggRequest.isOK()) {
                return viewAggRequest.getStatus();
            }

            return runAggregate(
                opCtx, nss, viewAggRequest.getValue(), viewAggregation.getValue(), *out);
        }

        Collection* const collection = ctx->getCollection();

        auto executor =
            uassertStatusOK(getExecutorDistinct(opCtx, collection, nss.ns(), &parsedDistinct));

        Explain::explainStages(executor.get(), collection, verbosity, out);
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        // Acquire locks and resolve possible UUID. The RAII object is optional, because in the case
        // of a view, the locks need to be released.
        boost::optional<AutoGetCollectionForReadCommand> ctx;
        ctx.emplace(opCtx,
                    CommandHelpers::parseNsOrUUID(dbname, cmdObj),
                    AutoGetCollection::ViewMode::kViewsPermitted);
        const auto& nss = ctx->getNss();

        const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
        auto parsedDistinct =
            uassertStatusOK(ParsedDistinct::parse(opCtx, nss, cmdObj, extensionsCallback, false));

        // Check whether we are allowed to read from this node after acquiring our locks.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassertStatusOK(replCoord->checkCanServeReadsFor(
            opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

        if (ctx->getView()) {
            // Relinquish locks. The aggregation command will re-acquire them.
            ctx.reset();

            auto viewAggregation = parsedDistinct.asAggregationCommand();
            uassertStatusOK(viewAggregation.getStatus());

            BSONObj aggResult = CommandHelpers::runCommandDirectly(
                opCtx, OpMsgRequest::fromDBAndBody(dbname, std::move(viewAggregation.getValue())));
            uassertStatusOK(ViewResponseFormatter(aggResult).appendAsDistinctResponse(&result));
            return true;
        }

        Collection* const collection = ctx->getCollection();

        auto executor = getExecutorDistinct(opCtx, collection, nss.ns(), &parsedDistinct);
        uassertStatusOK(executor.getStatus());

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setPlanSummary_inlock(
                Explain::getPlanSummary(executor.getValue().get()));
        }

        const auto key = cmdObj[ParsedDistinct::kKeyField].valuestrsafe();

        int bufSize = BSONObjMaxUserSize - 4096;
        BufBuilder bb(bufSize);
        char* start = bb.buf();

        BSONArrayBuilder arr(bb);
        BSONElementSet values(executor.getValue()->getCanonicalQuery()->getCollator());

        BSONObj obj;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = executor.getValue()->getNext(&obj, NULL))) {
            // Distinct expands arrays.
            //
            // If our query is covered, each value of the key should be in the index key and
            // available to us without this.  If a collection scan is providing the data, we may
            // have to expand an array.
            BSONElementSet elts;
            dps::extractAllElementsAlongPath(obj, key, elts);

            for (BSONElementSet::iterator it = elts.begin(); it != elts.end(); ++it) {
                BSONElement elt = *it;
                if (values.count(elt)) {
                    continue;
                }
                int currentBufPos = bb.len();

                uassert(17217,
                        "distinct too big, 16mb cap",
                        (currentBufPos + elt.size() + 1024) < bufSize);

                arr.append(elt);
                BSONElement x(start + currentBufPos);
                values.insert(x);
            }
        }

        // Return an error if execution fails for any reason.
        if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
            log() << "Plan executor error during distinct command: "
                  << redact(PlanExecutor::statestr(state))
                  << ", stats: " << redact(Explain::getWinningPlanStats(executor.getValue().get()));

            uassertStatusOK(WorkingSetCommon::getMemberObjectStatus(obj).withContext(
                "Executor error during distinct command"));
        }


        auto curOp = CurOp::get(opCtx);

        // Get summary information about the plan.
        PlanSummaryStats stats;
        Explain::getSummaryStats(*executor.getValue(), &stats);
        if (collection) {
            collection->infoCache()->notifyOfQuery(opCtx, stats.indexesUsed);
        }
        curOp->debug().setPlanSummaryMetrics(stats);

        if (curOp->shouldDBProfile()) {
            BSONObjBuilder execStatsBob;
            Explain::getWinningPlanStats(executor.getValue().get(), &execStatsBob);
            curOp->debug().execStats = execStatsBob.obj();
        }

        verify(start == bb.buf());

        result.appendArray("values", arr.done());

        return true;
    }

} distinctCmd;

}  // namespace
}  // namespace mongo
