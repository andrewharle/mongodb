/**
 *    Copyright (C) 2012-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands/find_and_modify.h"

#include <memory>
#include <boost/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/d_state.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

const UpdateStats* getUpdateStats(const PlanExecutor* exec) {
    // The stats may refer to an update stage, or a projection stage wrapping an update stage.
    if (StageType::STAGE_PROJECTION == exec->getRootStage()->stageType()) {
        invariant(exec->getRootStage()->getChildren().size() == 1U);
        invariant(StageType::STAGE_UPDATE == exec->getRootStage()->child()->stageType());
        const SpecificStats* stats = exec->getRootStage()->child()->getSpecificStats();
        return static_cast<const UpdateStats*>(stats);
    } else {
        invariant(StageType::STAGE_UPDATE == exec->getRootStage()->stageType());
        return static_cast<const UpdateStats*>(exec->getRootStage()->getSpecificStats());
    }
}

const DeleteStats* getDeleteStats(const PlanExecutor* exec) {
    // The stats may refer to a delete stage, or a projection stage wrapping a delete stage.
    if (StageType::STAGE_PROJECTION == exec->getRootStage()->stageType()) {
        invariant(exec->getRootStage()->getChildren().size() == 1U);
        invariant(StageType::STAGE_DELETE == exec->getRootStage()->child()->stageType());
        const SpecificStats* stats = exec->getRootStage()->child()->getSpecificStats();
        return static_cast<const DeleteStats*>(stats);
    } else {
        invariant(StageType::STAGE_DELETE == exec->getRootStage()->stageType());
        return static_cast<const DeleteStats*>(exec->getRootStage()->getSpecificStats());
    }
}

/**
 * If the operation succeeded, then Status::OK() is returned, possibly with a document value
 * to return to the client. If no matching document to update or remove was found, then none
 * is returned. Otherwise, the updated or deleted document is returned.
 *
 * If the operation failed, then an error Status is returned.
 */
StatusWith<boost::optional<BSONObj>> advanceExecutor(OperationContext* txn,
                                                     PlanExecutor* exec,
                                                     bool isRemove) {
    BSONObj value;
    PlanExecutor::ExecState state = exec->getNext(&value, nullptr);

    if (PlanExecutor::ADVANCED == state) {
        return boost::optional<BSONObj>(std::move(value));
    }

    if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
        error() << "Plan executor error during findAndModify: " << PlanExecutor::statestr(state)
                << ", stats: " << Explain::getWinningPlanStats(exec);

        if (WorkingSetCommon::isValidStatusMemberObject(value)) {
            const Status errorStatus = WorkingSetCommon::getMemberObjectStatus(value);
            invariant(!errorStatus.isOK());
            return {errorStatus.code(), errorStatus.reason()};
        }
        const std::string opstr = isRemove ? "delete" : "update";
        return {ErrorCodes::OperationFailed,
                str::stream() << "executor returned " << PlanExecutor::statestr(state)
                              << " while executing " << opstr};
    }

    invariant(state == PlanExecutor::IS_EOF);
    return boost::optional<BSONObj>(boost::none);
}

void makeUpdateRequest(const FindAndModifyRequest& args,
                       bool explain,
                       UpdateLifecycleImpl* updateLifecycle,
                       UpdateRequest* requestOut) {
    requestOut->setQuery(args.getQuery());
    requestOut->setProj(args.getFields());
    requestOut->setUpdates(args.getUpdateObj());
    requestOut->setSort(args.getSort());
    requestOut->setUpsert(args.isUpsert());
    requestOut->setReturnDocs(args.shouldReturnNew() ? UpdateRequest::RETURN_NEW
                                                     : UpdateRequest::RETURN_OLD);
    requestOut->setMulti(false);
    requestOut->setYieldPolicy(PlanExecutor::YIELD_AUTO);
    requestOut->setExplain(explain);
    requestOut->setLifecycle(updateLifecycle);
}

void makeDeleteRequest(const FindAndModifyRequest& args, bool explain, DeleteRequest* requestOut) {
    requestOut->setQuery(args.getQuery());
    requestOut->setProj(args.getFields());
    requestOut->setSort(args.getSort());
    requestOut->setMulti(false);
    requestOut->setYieldPolicy(PlanExecutor::YIELD_AUTO);
    requestOut->setReturnDeleted(true);  // Always return the old value.
    requestOut->setExplain(explain);
}

void appendCommandResponse(PlanExecutor* exec,
                           bool isRemove,
                           const boost::optional<BSONObj>& value,
                           BSONObjBuilder& result) {
    BSONObjBuilder lastErrorObjBuilder(result.subobjStart("lastErrorObject"));

    if (isRemove) {
        lastErrorObjBuilder.appendNumber("n", getDeleteStats(exec)->docsDeleted);
    } else {
        const UpdateStats* updateStats = getUpdateStats(exec);
        lastErrorObjBuilder.appendBool("updatedExisting", updateStats->nMatched > 0);
        lastErrorObjBuilder.appendNumber("n", updateStats->inserted ? 1 : updateStats->nMatched);
        // Note we have to use the objInserted from the stats here, rather than 'value'
        // because the _id field could have been excluded by a projection.
        if (!updateStats->objInserted.isEmpty()) {
            lastErrorObjBuilder.appendAs(updateStats->objInserted["_id"], kUpsertedFieldName);
        }
    }
    lastErrorObjBuilder.done();

    if (value) {
        result.append("value", *value);
    } else {
        result.appendNull("value");
    }
}

Status checkCanAcceptWritesForDatabase(const NamespaceString& nsString) {
    if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nsString)) {
        return Status(ErrorCodes::NotMaster,
                      str::stream()
                          << "Not primary while running findAndModify command on collection "
                          << nsString.ns());
    }
    return Status::OK();
}

}  // namespace

/* Find and Modify an object returning either the old (default) or new value*/
class CmdFindAndModify : public Command {
public:
    void help(std::stringstream& help) const override {
        help << "{ findAndModify: \"collection\", query: {processed:false}, update: {$set: "
                "{processed:true}}, new: true}\n"
                "{ findAndModify: \"collection\", query: {processed:false}, remove: true, sort: "
                "{priority:-1}}\n"
                "Either update or remove is required, all other fields have default values.\n"
                "Output is in the \"value\" field\n";
    }

    CmdFindAndModify() : Command("findAndModify", false, "findandmodify") {}
    bool slaveOk() const override {
        return false;
    }
    bool isWriteCommandForConfigServer() const override {
        return true;
    }
    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        find_and_modify::addPrivilegesRequiredForFindAndModify(this, dbname, cmdObj, out);
    }

    Status explain(OperationContext* txn,
                   const std::string& dbName,
                   const BSONObj& cmdObj,
                   ExplainCommon::Verbosity verbosity,
                   const rpc::ServerSelectionMetadata&,
                   BSONObjBuilder* out) const override {
        const std::string fullNs = parseNsCollectionRequired(dbName, cmdObj);
        Status allowedWriteStatus = userAllowedWriteNS(fullNs);
        if (!allowedWriteStatus.isOK()) {
            return allowedWriteStatus;
        }

        StatusWith<FindAndModifyRequest> parseStatus =
            FindAndModifyRequest::parseFromBSON(NamespaceString(fullNs), cmdObj);
        if (!parseStatus.isOK()) {
            return parseStatus.getStatus();
        }

        const FindAndModifyRequest& args = parseStatus.getValue();
        const NamespaceString& nsString = args.getNamespaceString();

        if (args.isRemove()) {
            DeleteRequest request(nsString);
            const bool isExplain = true;
            makeDeleteRequest(args, isExplain, &request);

            ParsedDelete parsedDelete(txn, &request);
            Status parsedDeleteStatus = parsedDelete.parseRequest();
            if (!parsedDeleteStatus.isOK()) {
                return parsedDeleteStatus;
            }

            // Explain calls of the findAndModify command are read-only, but we take write
            // locks so that the timing information is more accurate.
            AutoGetDb autoDb(txn, dbName, MODE_IX);
            Lock::CollectionLock collLock(txn->lockState(), nsString.ns(), MODE_IX);

            ensureShardVersionOKOrThrow(txn, nsString.ns());

            Collection* collection = nullptr;
            if (autoDb.getDb()) {
                collection = autoDb.getDb()->getCollection(nsString.ns());
            } else {
                return {ErrorCodes::NamespaceNotFound,
                        str::stream() << "database " << dbName << " does not exist."};
            }

            auto statusWithPlanExecutor = getExecutorDelete(txn, collection, &parsedDelete);
            if (!statusWithPlanExecutor.isOK()) {
                return statusWithPlanExecutor.getStatus();
            }
            const std::unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());
            Explain::explainStages(exec.get(), verbosity, out);
        } else {
            UpdateRequest request(nsString);
            const bool ignoreVersion = false;
            UpdateLifecycleImpl updateLifecycle(ignoreVersion, nsString);
            const bool isExplain = true;
            makeUpdateRequest(args, isExplain, &updateLifecycle, &request);

            ParsedUpdate parsedUpdate(txn, &request);
            Status parsedUpdateStatus = parsedUpdate.parseRequest();
            if (!parsedUpdateStatus.isOK()) {
                return parsedUpdateStatus;
            }

            OpDebug* opDebug = &CurOp::get(txn)->debug();

            // Explain calls of the findAndModify command are read-only, but we take write
            // locks so that the timing information is more accurate.
            AutoGetDb autoDb(txn, dbName, MODE_IX);
            Lock::CollectionLock collLock(txn->lockState(), nsString.ns(), MODE_IX);

            ensureShardVersionOKOrThrow(txn, nsString.ns());

            Collection* collection = nullptr;
            if (autoDb.getDb()) {
                collection = autoDb.getDb()->getCollection(nsString.ns());
            } else {
                return {ErrorCodes::NamespaceNotFound,
                        str::stream() << "database " << dbName << " does not exist."};
            }

            auto statusWithPlanExecutor =
                getExecutorUpdate(txn, collection, &parsedUpdate, opDebug);
            if (!statusWithPlanExecutor.isOK()) {
                return statusWithPlanExecutor.getStatus();
            }
            const std::unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());
            Explain::explainStages(exec.get(), verbosity, out);
        }

        return Status::OK();
    }

    bool run(OperationContext* txn,
             const std::string& dbName,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        // findAndModify command is not replicated directly.
        invariant(txn->writesAreReplicated());
        const std::string fullNs = parseNsCollectionRequired(dbName, cmdObj);
        Status allowedWriteStatus = userAllowedWriteNS(fullNs);
        if (!allowedWriteStatus.isOK()) {
            return appendCommandStatus(result, allowedWriteStatus);
        }

        StatusWith<FindAndModifyRequest> parseStatus =
            FindAndModifyRequest::parseFromBSON(NamespaceString(fullNs), cmdObj);
        if (!parseStatus.isOK()) {
            return appendCommandStatus(result, parseStatus.getStatus());
        }

        const FindAndModifyRequest& args = parseStatus.getValue();
        const NamespaceString& nsString = args.getNamespaceString();

        StatusWith<WriteConcernOptions> wcResult = extractWriteConcern(txn, cmdObj, dbName);
        if (!wcResult.isOK()) {
            return appendCommandStatus(result, wcResult.getStatus());
        }
        txn->setWriteConcern(wcResult.getValue());
        setupSynchronousCommit(txn);

        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj))
            maybeDisableValidation.emplace(txn);

        auto client = txn->getClient();
        auto lastOpAtOperationStart = repl::ReplClientInfo::forClient(client).getLastOp();
        ScopeGuard lastOpSetterGuard =
            MakeObjGuard(repl::ReplClientInfo::forClient(client),
                         &repl::ReplClientInfo::setLastOpToSystemLastOpTime,
                         txn);

        // Although usually the PlanExecutor handles WCE internally, it will throw WCEs when it is
        // executing a findAndModify. This is done to ensure that we can always match, modify, and
        // return the document under concurrency, if a matching document exists.
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            if (args.isRemove()) {
                DeleteRequest request(nsString);
                const bool isExplain = false;
                makeDeleteRequest(args, isExplain, &request);

                ParsedDelete parsedDelete(txn, &request);
                Status parsedDeleteStatus = parsedDelete.parseRequest();
                if (!parsedDeleteStatus.isOK()) {
                    return appendCommandStatus(result, parsedDeleteStatus);
                }

                AutoGetOrCreateDb autoDb(txn, dbName, MODE_IX);
                Lock::CollectionLock collLock(txn->lockState(), nsString.ns(), MODE_IX);
                Collection* collection = autoDb.getDb()->getCollection(nsString.ns());

                // Attach the namespace and database profiling level to the current op.
                {
                    stdx::lock_guard<Client> lk(*txn->getClient());
                    CurOp::get(txn)
                        ->enter_inlock(nsString.ns().c_str(), autoDb.getDb()->getProfilingLevel());
                }

                ensureShardVersionOKOrThrow(txn, nsString.ns());

                Status isPrimary = checkCanAcceptWritesForDatabase(nsString);
                if (!isPrimary.isOK()) {
                    return appendCommandStatus(result, isPrimary);
                }

                auto statusWithPlanExecutor = getExecutorDelete(txn, collection, &parsedDelete);
                if (!statusWithPlanExecutor.isOK()) {
                    return appendCommandStatus(result, statusWithPlanExecutor.getStatus());
                }
                const std::unique_ptr<PlanExecutor> exec =
                    std::move(statusWithPlanExecutor.getValue());

                StatusWith<boost::optional<BSONObj>> advanceStatus =
                    advanceExecutor(txn, exec.get(), args.isRemove());
                if (!advanceStatus.isOK()) {
                    return appendCommandStatus(result, advanceStatus.getStatus());
                }

                PlanSummaryStats summaryStats;
                Explain::getSummaryStats(*exec, &summaryStats);
                if (collection) {
                    collection->infoCache()->notifyOfQuery(txn, summaryStats.indexesUsed);
                }
                CurOp::get(txn)->debug().fromMultiPlanner = summaryStats.fromMultiPlanner;
                CurOp::get(txn)->debug().replanned = summaryStats.replanned;

                // Fill out OpDebug with the number of deleted docs.
                CurOp::get(txn)->debug().ndeleted = getDeleteStats(exec.get())->docsDeleted;

                boost::optional<BSONObj> value = advanceStatus.getValue();
                appendCommandResponse(exec.get(), args.isRemove(), value, result);
            } else {
                UpdateRequest request(nsString);
                const bool ignoreVersion = false;
                UpdateLifecycleImpl updateLifecycle(ignoreVersion, nsString);
                const bool isExplain = false;
                makeUpdateRequest(args, isExplain, &updateLifecycle, &request);

                ParsedUpdate parsedUpdate(txn, &request);
                Status parsedUpdateStatus = parsedUpdate.parseRequest();
                if (!parsedUpdateStatus.isOK()) {
                    return appendCommandStatus(result, parsedUpdateStatus);
                }

                OpDebug* opDebug = &CurOp::get(txn)->debug();

                AutoGetOrCreateDb autoDb(txn, dbName, MODE_IX);
                Lock::CollectionLock collLock(txn->lockState(), nsString.ns(), MODE_IX);
                Collection* collection = autoDb.getDb()->getCollection(nsString.ns());

                // Attach the namespace and database profiling level to the current op.
                {
                    stdx::lock_guard<Client> lk(*txn->getClient());
                    CurOp::get(txn)
                        ->enter_inlock(nsString.ns().c_str(), autoDb.getDb()->getProfilingLevel());
                }

                ensureShardVersionOKOrThrow(txn, nsString.ns());

                Status isPrimary = checkCanAcceptWritesForDatabase(nsString);
                if (!isPrimary.isOK()) {
                    return appendCommandStatus(result, isPrimary);
                }

                // Create the collection if it does not exist when performing an upsert
                // because the update stage does not create its own collection.
                if (!collection && args.isUpsert()) {
                    // Release the collection lock and reacquire a lock on the database
                    // in exclusive mode in order to create the collection.
                    collLock.relockAsDatabaseExclusive(autoDb.lock());
                    collection = autoDb.getDb()->getCollection(nsString.ns());
                    Status isPrimaryAfterRelock = checkCanAcceptWritesForDatabase(nsString);
                    if (!isPrimaryAfterRelock.isOK()) {
                        return appendCommandStatus(result, isPrimaryAfterRelock);
                    }

                    if (collection) {
                        // Someone else beat us to creating the collection, do nothing.
                    } else {
                        WriteUnitOfWork wuow(txn);
                        Status createCollStatus =
                            userCreateNS(txn, autoDb.getDb(), nsString.ns(), BSONObj());
                        if (!createCollStatus.isOK()) {
                            return appendCommandStatus(result, createCollStatus);
                        }
                        wuow.commit();

                        collection = autoDb.getDb()->getCollection(nsString.ns());
                        invariant(collection);
                    }
                }

                auto statusWithPlanExecutor =
                    getExecutorUpdate(txn, collection, &parsedUpdate, opDebug);
                if (!statusWithPlanExecutor.isOK()) {
                    return appendCommandStatus(result, statusWithPlanExecutor.getStatus());
                }
                const std::unique_ptr<PlanExecutor> exec =
                    std::move(statusWithPlanExecutor.getValue());

                StatusWith<boost::optional<BSONObj>> advanceStatus =
                    advanceExecutor(txn, exec.get(), args.isRemove());
                if (!advanceStatus.isOK()) {
                    return appendCommandStatus(result, advanceStatus.getStatus());
                }

                PlanSummaryStats summaryStats;
                Explain::getSummaryStats(*exec, &summaryStats);
                if (collection) {
                    collection->infoCache()->notifyOfQuery(txn, summaryStats.indexesUsed);
                }
                UpdateStage::fillOutOpDebug(getUpdateStats(exec.get()), &summaryStats, opDebug);

                boost::optional<BSONObj> value = advanceStatus.getValue();
                appendCommandResponse(exec.get(), args.isRemove(), value, result);
            }
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "findAndModify", nsString.ns());

        if (repl::ReplClientInfo::forClient(client).getLastOp() != lastOpAtOperationStart) {
            // If this operation has already generated a new lastOp, don't bother setting it here.
            // No-op updates will not generate a new lastOp, so we still need the guard to fire in
            // that case.
            lastOpSetterGuard.Dismiss();
        }

        WriteConcernResult res;
        auto waitForWCStatus =
            waitForWriteConcern(txn,
                                repl::ReplClientInfo::forClient(txn->getClient()).getLastOp(),
                                txn->getWriteConcern(),
                                &res);
        appendCommandWCStatus(result, waitForWCStatus);

        return true;
    }

} cmdFindAndModify;

}  // namespace mongo
