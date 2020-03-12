
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_read_concern_metrics.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto kTermField = "term"_sd;

/**
 * A command for running .find() queries.
 */
class FindCmd : public BasicCommand {
public:
    FindCmd() : BasicCommand("find") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const final {
        return true;
    }

    std::string help() const override {
        return "query for documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opQuery;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
    }

    /**
     * A find command does not increment the command counter, but rather increments the
     * query counter.
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

        if (!authSession->isAuthorizedToParseNamespaceElement(cmdObj.firstElement())) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        const auto hasTerm = cmdObj.hasField(kTermField);
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

        // Parse the command BSON to a QueryRequest.
        const bool isExplain = true;
        auto qrStatus = QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain);
        if (!qrStatus.isOK()) {
            return qrStatus.getStatus();
        }

        // Finish the parsing step by using the QueryRequest to create a CanonicalQuery.
        const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
        const boost::intrusive_ptr<ExpressionContext> expCtx;
        auto statusWithCQ =
            CanonicalQuery::canonicalize(opCtx,
                                         std::move(qrStatus.getValue()),
                                         expCtx,
                                         extensionsCallback,
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        if (!statusWithCQ.isOK()) {
            return statusWithCQ.getStatus();
        }
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        if (ctx->getView()) {
            // Relinquish locks. The aggregation command will re-acquire them.
            ctx.reset();

            // Convert the find command into an aggregation using $match (and other stages, as
            // necessary), if possible.
            const auto& qr = cq->getQueryRequest();
            auto viewAggregationCommand = qr.asAggregationCommand();
            if (!viewAggregationCommand.isOK())
                return viewAggregationCommand.getStatus();

            // Create the agg request equivalent of the find operation, with the explain verbosity
            // included.
            auto aggRequest = AggregationRequest::parseFromBSON(
                nss, viewAggregationCommand.getValue(), verbosity);
            if (!aggRequest.isOK()) {
                return aggRequest.getStatus();
            }

            try {
                return runAggregate(
                    opCtx, nss, aggRequest.getValue(), viewAggregationCommand.getValue(), *out);
            } catch (DBException& error) {
                if (error.code() == ErrorCodes::InvalidPipelineOperator) {
                    return {ErrorCodes::InvalidPipelineOperator,
                            str::stream() << "Unsupported in view pipeline: " << error.what()};
                }
                return error.toStatus();
            }
        }

        // The collection may be NULL. If so, getExecutor() should handle it by returning an
        // execution tree with an EOFStage.
        Collection* const collection = ctx->getCollection();

        // We have a parsed query. Time to get the execution plan for it.
        auto statusWithPlanExecutor = getExecutorFind(opCtx, collection, std::move(cq));
        if (!statusWithPlanExecutor.isOK()) {
            return statusWithPlanExecutor.getStatus();
        }
        auto exec = std::move(statusWithPlanExecutor.getValue());

        // Got the execution tree. Explain it.
        Explain::explainStages(exec.get(), collection, verbosity, out);
        return Status::OK();
    }

    /**
     * Runs a query using the following steps:
     *   --Parsing.
     *   --Acquire locks.
     *   --Plan query, obtaining an executor that can run it.
     *   --Generate the first batch.
     *   --Save state for getMore, transferring ownership of the executor to a ClientCursor.
     *   --Generate response to send to the client.
     */
    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        // Although it is a command, a find command gets counted as a query.
        globalOpCounters.gotQuery();
        ServerReadConcernMetrics::get(opCtx)->recordReadConcern(repl::ReadConcernArgs::get(opCtx));

        // Parse the command BSON to a QueryRequest.
        const bool isExplain = false;
        // Pass parseNs to makeFromFindCommand in case cmdObj does not have a UUID.
        auto qrStatus = QueryRequest::makeFromFindCommand(
            NamespaceString(parseNs(dbname, cmdObj)), cmdObj, isExplain);
        uassertStatusOK(qrStatus.getStatus());

        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        auto& qr = qrStatus.getValue();
        const auto session = OperationContextSession::get(opCtx);
        uassert(ErrorCodes::InvalidOptions,
                "It is illegal to open a tailable cursor in a transaction",
                session == nullptr ||
                    !(session->inActiveOrKilledMultiDocumentTransaction() && qr->isTailable()));

        // Validate term before acquiring locks, if provided.
        if (auto term = qr->getReplicationTerm()) {
            Status status = replCoord->updateTerm(opCtx, *term);
            // Note: updateTerm returns ok if term stayed the same.
            uassertStatusOK(status);
        }

        // Acquire locks. If the query is on a view, we release our locks and convert the query
        // request into an aggregation command.
        boost::optional<AutoGetCollectionForReadCommand> ctx;
        ctx.emplace(opCtx,
                    CommandHelpers::parseNsOrUUID(dbname, cmdObj),
                    AutoGetCollection::ViewMode::kViewsPermitted);
        const auto& nss = ctx->getNss();

        qr->refreshNSS(opCtx);

        // Check whether we are allowed to read from this node after acquiring our locks.
        uassertStatusOK(replCoord->checkCanServeReadsFor(
            opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

        // Fill out curop information.
        //
        // We pass negative values for 'ntoreturn' and 'ntoskip' to indicate that these values
        // should be omitted from the log line. Limit and skip information is already present in the
        // find command parameters, so these fields are redundant.
        const int ntoreturn = -1;
        const int ntoskip = -1;
        beginQueryOp(opCtx, nss, cmdObj, ntoreturn, ntoskip);

        // Finish the parsing step by using the QueryRequest to create a CanonicalQuery.
        const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
        const boost::intrusive_ptr<ExpressionContext> expCtx;
        auto statusWithCQ =
            CanonicalQuery::canonicalize(opCtx,
                                         std::move(qr),
                                         expCtx,
                                         extensionsCallback,
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        uassertStatusOK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        if (ctx->getView()) {
            // Relinquish locks. The aggregation command will re-acquire them.
            ctx.reset();

            // Convert the find command into an aggregation using $match (and other stages, as
            // necessary), if possible.
            const auto& qr = cq->getQueryRequest();
            auto viewAggregationCommand = qr.asAggregationCommand();
            uassertStatusOK(viewAggregationCommand.getStatus());

            BSONObj aggResult = CommandHelpers::runCommandDirectly(
                opCtx,
                OpMsgRequest::fromDBAndBody(dbname, std::move(viewAggregationCommand.getValue())));
            auto status = getStatusFromCommandResult(aggResult);
            if (status.code() == ErrorCodes::InvalidPipelineOperator) {
                uasserted(ErrorCodes::InvalidPipelineOperator,
                          str::stream() << "Unsupported in view pipeline: " << status.reason());
            }
            result.resetToEmpty();
            result.appendElements(aggResult);
            return status.isOK();
        }

        Collection* const collection = ctx->getCollection();

        // Get the execution plan for the query.
        auto statusWithPlanExecutor = getExecutorFind(opCtx, collection, std::move(cq));
        uassertStatusOK(statusWithPlanExecutor.getStatus());

        auto exec = std::move(statusWithPlanExecutor.getValue());

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
        }

        if (!collection) {
            // No collection. Just fill out curop indicating that there were zero results and
            // there is no ClientCursor id, and then return.
            const long long numResults = 0;
            const CursorId cursorId = 0;
            endQueryOp(opCtx, collection, *exec, numResults, cursorId);
            appendCursorResponseObject(cursorId, nss.ns(), BSONArray(), &result);
            return true;
        }

        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &waitInFindBeforeMakingBatch, opCtx, "waitInFindBeforeMakingBatch");

        const QueryRequest& originalQR = exec->getCanonicalQuery()->getQueryRequest();

        // Stream query results, adding them to a BSONArray as we go.
        CursorResponseBuilder firstBatch(/*isInitialResponse*/ true, &result);
        BSONObj obj;
        PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
        long long numResults = 0;
        while (!FindCommon::enoughForFirstBatch(originalQR, numResults) &&
               PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
            // If we can't fit this result inside the current batch, then we stash it for later.
            if (!FindCommon::haveSpaceForNext(obj, numResults, firstBatch.bytesUsed())) {
                exec->enqueue(obj);
                break;
            }

            // Add result to output buffer.
            firstBatch.append(obj);
            numResults++;
        }

        // Throw an assertion if query execution fails for any reason.
        if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
            firstBatch.abandon();
            LOG(1) << "Plan executor error during find command: " << PlanExecutor::statestr(state)
                   << ", stats: " << redact(Explain::getWinningPlanStats(exec.get()));

            uassertStatusOK(WorkingSetCommon::getMemberObjectStatus(obj).withContext(
                "Executor error during find command"));
        }

        // Before saving the cursor, ensure that whatever plan we established happened with the
        // expected collection version
        auto css = CollectionShardingState::get(opCtx, nss);
        css->checkShardVersionOrThrow(opCtx);

        // Set up the cursor for getMore.
        CursorId cursorId = 0;
        if (shouldSaveCursor(opCtx, collection, state, exec.get())) {
            // Create a ClientCursor containing this plan executor and register it with the cursor
            // manager.
            ClientCursorPin pinnedCursor = collection->getCursorManager()->registerCursor(
                opCtx,
                {std::move(exec),
                 nss,
                 AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
                 repl::ReadConcernArgs::get(opCtx).getLevel(),
                 cmdObj});
            cursorId = pinnedCursor.getCursor()->cursorid();

            invariant(!exec);
            PlanExecutor* cursorExec = pinnedCursor.getCursor()->getExecutor();

            // State will be restored on getMore.
            cursorExec->saveState();
            cursorExec->detachFromOperationContext();

            // We assume that cursors created through a DBDirectClient are always used from their
            // original OperationContext, so we do not need to move time to and from the cursor.
            if (!opCtx->getClient()->isInDirectClient()) {
                pinnedCursor.getCursor()->setLeftoverMaxTimeMicros(
                    opCtx->getRemainingMaxTimeMicros());
            }
            pinnedCursor.getCursor()->setPos(numResults);

            // Fill out curop based on the results.
            endQueryOp(opCtx, collection, *cursorExec, numResults, cursorId);
        } else {
            endQueryOp(opCtx, collection, *exec, numResults, cursorId);
        }

        // Generate the response object to send to the client.
        firstBatch.done(cursorId, nss.ns());
        return true;
    }

} findCmd;

}  // namespace
}  // namespace mongo
