
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/repl/apply_ops.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

constexpr StringData ApplyOps::kPreconditionFieldName;
constexpr StringData ApplyOps::kOplogApplicationModeFieldName;

namespace {

// If enabled, causes loop in _applyOps() to hang after applying current operation.
MONGO_FAIL_POINT_DEFINE(applyOpsPauseBetweenOperations);

/**
 * Return true iff the applyOpsCmd can be executed in a single WriteUnitOfWork.
 */
bool _parseAreOpsCrudOnly(const BSONObj& applyOpCmd) {
    for (const auto& elem : applyOpCmd.firstElement().Obj()) {
        const char* names[] = {"ns", "op"};
        BSONElement fields[2];
        elem.Obj().getFields(2, names, fields);
        BSONElement& fieldNs = fields[0];
        BSONElement& fieldOp = fields[1];

        const char* opType = fieldOp.valuestrsafe();
        const StringData ns = fieldNs.valuestrsafe();

        // All atomic ops have an opType of length 1.
        if (opType[0] == '\0' || opType[1] != '\0')
            return false;

        // Only consider CRUD operations.
        switch (*opType) {
            case 'd':
            case 'n':
            case 'u':
                break;
            case 'i':
                if (nsToCollectionSubstring(ns) != "system.indexes")
                    break;
            // Fallthrough.
            default:
                return false;
        }
    }

    return true;
}

Status _applyOps(OperationContext* opCtx,
                 const std::string& dbName,
                 const BSONObj& applyOpCmd,
                 const ApplyOpsCommandInfo& info,
                 repl::OplogApplication::Mode oplogApplicationMode,
                 BSONObjBuilder* result,
                 int* numApplied,
                 BSONArrayBuilder* opsBuilder) {
    const auto& ops = info.getOperations();
    // apply
    *numApplied = 0;
    int errors = 0;

    BSONArrayBuilder ab;
    const auto& alwaysUpsert = info.getAlwaysUpsert();
    const bool haveWrappingWUOW = opCtx->lockState()->inAWriteUnitOfWork();

    // Apply each op in the given 'applyOps' command object.
    for (const auto& opObj : ops) {
        // Ignore 'n' operations.
        const char* opType = opObj["op"].valuestrsafe();
        if (*opType == 'n')
            continue;

        const NamespaceString nss(opObj["ns"].String());

        // Need to check this here, or OldClientContext may fail an invariant.
        if (*opType != 'c' && !nss.isValid())
            return {ErrorCodes::InvalidNamespace, "invalid ns: " + nss.ns()};

        Status status(ErrorCodes::InternalError, "");

        if (haveWrappingWUOW) {
            invariant(opCtx->lockState()->isW());
            invariant(*opType != 'c');

            auto db = DatabaseHolder::getDatabaseHolder().get(opCtx, nss.ns());
            if (!db) {
                // Retry in non-atomic mode, since MMAP cannot implicitly create a new database
                // within an active WriteUnitOfWork.
                uasserted(ErrorCodes::AtomicityFailure,
                          "cannot create a database in atomic applyOps mode; will retry without "
                          "atomicity");
            }

            // When processing an update on a non-existent collection, applyOperation_inlock()
            // returns UpdateOperationFailed on updates and allows the collection to be
            // implicitly created on upserts. We detect both cases here and fail early with
            // NamespaceNotFound.
            // Additionally for inserts, we fail early on non-existent collections.
            auto collection = db->getCollection(opCtx, nss);
            if (!collection && !nss.isSystemDotIndexes() && (*opType == 'i' || *opType == 'u')) {
                uasserted(
                    ErrorCodes::AtomicityFailure,
                    str::stream()
                        << "cannot apply insert or update operation on a non-existent namespace "
                        << nss.ns()
                        << " in atomic applyOps mode: "
                        << redact(opObj));
            }

            // Reject malformed operations in an atomic applyOps.
            try {
                ReplOperation::parse(IDLParserErrorContext("applyOps"), opObj);
            } catch (...) {
                uasserted(ErrorCodes::AtomicityFailure,
                          str::stream()
                              << "cannot apply a malformed operation in atomic applyOps mode: "
                              << redact(opObj)
                              << "; will retry without atomicity: "
                              << exceptionToStatus().toString());
            }

            OldClientContext ctx(opCtx, nss.ns());

            status = repl::applyOperation_inlock(
                opCtx, ctx.db(), opObj, alwaysUpsert, oplogApplicationMode);
            if (!status.isOK())
                return status;

            // Append completed op, including UUID if available, to 'opsBuilder'.
            if (opsBuilder) {
                if (opObj.hasField("ui") || nss.isSystemDotIndexes() ||
                    !(collection && collection->uuid())) {
                    // No changes needed to operation document.
                    opsBuilder->append(opObj);
                } else {
                    // Operation document has no "ui" field and collection has a UUID.
                    auto uuid = collection->uuid();
                    BSONObjBuilder opBuilder;
                    opBuilder.appendElements(opObj);
                    uuid->appendToBuilder(&opBuilder, "ui");
                    opsBuilder->append(opBuilder.obj());
                }
            }
        } else {
            try {
                status = writeConflictRetry(
                    opCtx,
                    "applyOps",
                    nss.ns(),
                    [opCtx, nss, opObj, opType, alwaysUpsert, oplogApplicationMode] {
                        if (*opType == 'c') {
                            invariant(opCtx->lockState()->isW());
                            uassertStatusOK(
                                repl::applyCommand_inlock(opCtx, opObj, oplogApplicationMode));
                            return Status::OK();
                        }

                        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
                        if (!autoColl.getCollection() && !nss.isSystemDotIndexes()) {
                            // For idempotency reasons, return success on delete operations.
                            if (*opType == 'd') {
                                return Status::OK();
                            }
                            uasserted(ErrorCodes::NamespaceNotFound,
                                      str::stream()
                                          << "cannot apply insert or update operation on a "
                                             "non-existent namespace "
                                          << nss.ns()
                                          << ": "
                                          << mongo::redact(opObj));
                        }

                        OldClientContext ctx(opCtx, nss.ns());

                        if (!nss.isSystemDotIndexes()) {
                            // We return the status rather than merely aborting so failure of CRUD
                            // ops doesn't stop the applyOps from trying to process the rest of the
                            // ops.  This is to leave the door open to parallelizing CRUD op
                            // application in the future.
                            return repl::applyOperation_inlock(
                                opCtx, ctx.db(), opObj, alwaysUpsert, oplogApplicationMode);
                        }

                        auto fieldO = opObj["o"];
                        BSONObj indexSpec;
                        NamespaceString indexNss;
                        std::tie(indexSpec, indexNss) =
                            repl::prepForApplyOpsIndexInsert(fieldO, opObj, nss);
                        BSONObjBuilder command;
                        command.append("createIndexes", indexNss.coll());
                        command.appendElements(indexSpec);
                        const BSONObj commandObj = command.done();

                        BSONObjBuilder createIndexesOp;
                        createIndexesOp.append("op", "c");
                        createIndexesOp.append("ns", nss.getCommandNS().toString());
                        createIndexesOp.append("o", commandObj);
                        auto createIndexesOpObj = createIndexesOp.done();

                        // Uassert to stop applyOps only when building indexes, but not for CRUD
                        // ops.
                        invariant(opCtx->lockState()->isW());
                        uassertStatusOK(repl::applyCommand_inlock(
                            opCtx, createIndexesOpObj, oplogApplicationMode));
                        return Status::OK();
                    });
            } catch (const DBException& ex) {
                ab.append(false);
                result->append("applied", ++(*numApplied));
                result->append("code", ex.code());
                result->append("codeName", ErrorCodes::errorString(ex.code()));
                result->append("errmsg", ex.what());
                result->append("results", ab.arr());
                return Status(ex.code(), ex.what());
            }
        }

        ab.append(status.isOK());
        if (!status.isOK()) {
            log() << "applyOps error applying: " << status;
            errors++;
        }

        (*numApplied)++;

        if (MONGO_FAIL_POINT(applyOpsPauseBetweenOperations)) {
            // While holding a database lock under MMAPv1, we would be implicitly holding the
            // flush lock here. This would prevent other threads from acquiring the global
            // lock or any database locks. We release all locks temporarily while the fail
            // point is enabled to allow other threads to make progress.
            boost::optional<Lock::TempRelease> release;
            auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
            if (storageEngine->isMmapV1() && !opCtx->lockState()->isW()) {
                release.emplace(opCtx->lockState());
            }
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(applyOpsPauseBetweenOperations);
        }
    }

    result->append("applied", *numApplied);
    result->append("results", ab.arr());

    if (errors != 0) {
        return Status(ErrorCodes::UnknownError, "applyOps had one or more errors applying ops");
    }

    return Status::OK();
}

Status _checkPrecondition(OperationContext* opCtx,
                          const std::vector<BSONObj>& preConditions,
                          BSONObjBuilder* result) {
    invariant(opCtx->lockState()->isW());

    for (const auto& preCondition : preConditions) {
        if (preCondition["ns"].type() != BSONType::String) {
            return {ErrorCodes::InvalidNamespace,
                    str::stream() << "ns in preCondition must be a string, but found type: "
                                  << typeName(preCondition["ns"].type())};
        }
        const NamespaceString nss(preCondition["ns"].valueStringData());
        if (!nss.isValid()) {
            return {ErrorCodes::InvalidNamespace, "invalid ns: " + nss.ns()};
        }

        DBDirectClient db(opCtx);
        BSONObj realres = db.findOne(nss.ns(), preCondition["q"].Obj());

        // Get collection default collation.
        Database* database = DatabaseHolder::getDatabaseHolder().get(opCtx, nss.db());
        if (!database) {
            return {ErrorCodes::NamespaceNotFound, "database in ns does not exist: " + nss.ns()};
        }
        Collection* collection = database->getCollection(opCtx, nss);
        if (!collection) {
            return {ErrorCodes::NamespaceNotFound, "collection in ns does not exist: " + nss.ns()};
        }
        const CollatorInterface* collator = collection->getDefaultCollator();

        // applyOps does not allow any extensions, such as $text, $where, $geoNear, $near,
        // $nearSphere, or $expr.
        boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(opCtx, collator));
        Matcher matcher(preCondition["res"].Obj(), std::move(expCtx));
        if (!matcher.matches(realres)) {
            result->append("got", realres);
            result->append("whatFailed", preCondition);
            return {ErrorCodes::BadValue, "preCondition failed"};
        }
    }

    return Status::OK();
}
}  // namespace

// static
ApplyOpsCommandInfo ApplyOpsCommandInfo::parse(const BSONObj& applyOpCmd) {
    try {
        return ApplyOpsCommandInfo(applyOpCmd);
    } catch (DBException& ex) {
        ex.addContext(str::stream() << "Failed to parse applyOps command: " << redact(applyOpCmd));
        throw;
    }
}

bool ApplyOpsCommandInfo::areOpsCrudOnly() const {
    return _areOpsCrudOnly;
}

bool ApplyOpsCommandInfo::isAtomic() const {
    return getAllowAtomic() && areOpsCrudOnly();
}

ApplyOpsCommandInfo::ApplyOpsCommandInfo(const BSONObj& applyOpCmd)
    : _areOpsCrudOnly(_parseAreOpsCrudOnly(applyOpCmd)) {
    parseProtected(IDLParserErrorContext("applyOps"), applyOpCmd);

    if (getPreCondition()) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot use preCondition with {allowAtomic: false}",
                getAllowAtomic());
        uassert(ErrorCodes::InvalidOptions,
                "Cannot use preCondition when operations include commands.",
                areOpsCrudOnly());
    }
}

Status applyOps(OperationContext* opCtx,
                const std::string& dbName,
                const BSONObj& applyOpCmd,
                repl::OplogApplication::Mode oplogApplicationMode,
                BSONObjBuilder* result) {
    auto info = ApplyOpsCommandInfo::parse(applyOpCmd);

    boost::optional<Lock::GlobalWrite> globalWriteLock;
    boost::optional<Lock::DBLock> dbWriteLock;

    // There's only one case where we are allowed to take the database lock instead of the global
    // lock - no preconditions; only CRUD ops; and non-atomic mode.
    if (!info.getPreCondition() && info.areOpsCrudOnly() && !info.getAllowAtomic()) {
        dbWriteLock.emplace(opCtx, dbName, MODE_IX);
    } else {
        globalWriteLock.emplace(opCtx);
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool userInitiatedWritesAndNotPrimary =
        opCtx->writesAreReplicated() && !replCoord->canAcceptWritesForDatabase(opCtx, dbName);

    if (userInitiatedWritesAndNotPrimary)
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while applying ops to database " << dbName);

    if (auto preCondition = info.getPreCondition()) {
        invariant(info.isAtomic());
        auto status = _checkPrecondition(opCtx, *preCondition, result);
        if (!status.isOK()) {
            return status;
        }
    }

    int numApplied = 0;
    if (!info.isAtomic()) {
        return _applyOps(
            opCtx, dbName, applyOpCmd, info, oplogApplicationMode, result, &numApplied, nullptr);
    }

    // Perform write ops atomically
    invariant(globalWriteLock);

    try {
        writeConflictRetry(opCtx, "applyOps", dbName, [&] {
            BSONObjBuilder intermediateResult;
            std::unique_ptr<BSONArrayBuilder> opsBuilder;
            if (opCtx->writesAreReplicated()) {
                opsBuilder = stdx::make_unique<BSONArrayBuilder>();
            }
            WriteUnitOfWork wunit(opCtx);
            numApplied = 0;
            {
                // Suppress replication for atomic operations until end of applyOps.
                repl::UnreplicatedWritesBlock uwb(opCtx);
                uassertStatusOK(_applyOps(opCtx,
                                          dbName,
                                          applyOpCmd,
                                          info,
                                          oplogApplicationMode,
                                          &intermediateResult,
                                          &numApplied,
                                          opsBuilder.get()));
            }
            // Generate oplog entry for all atomic ops collectively.
            if (opCtx->writesAreReplicated()) {
                // We want this applied atomically on slaves so we rewrite the oplog entry without
                // the pre-condition for speed.

                BSONObjBuilder cmdBuilder;

                auto opsFieldName = applyOpCmd.firstElement().fieldNameStringData();
                for (auto elem : applyOpCmd) {
                    auto name = elem.fieldNameStringData();
                    if (name == opsFieldName && opsBuilder) {
                        cmdBuilder.append(opsFieldName, opsBuilder->arr());
                        continue;
                    }
                    if (name == ApplyOps::kPreconditionFieldName)
                        continue;
                    if (name == bypassDocumentValidationCommandOption())
                        continue;
                    cmdBuilder.append(elem);
                }

                const BSONObj cmdRewritten = cmdBuilder.done();

                auto opObserver = getGlobalServiceContext()->getOpObserver();
                invariant(opObserver);
                opObserver->onApplyOps(opCtx, dbName, cmdRewritten);
            }
            wunit.commit();
            result->appendElements(intermediateResult.obj());
        });
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::AtomicityFailure) {
            // Retry in non-atomic mode.
            return _applyOps(opCtx,
                             dbName,
                             applyOpCmd,
                             info,
                             oplogApplicationMode,
                             result,
                             &numApplied,
                             nullptr);
        }
        BSONArrayBuilder ab;
        ++numApplied;
        for (int j = 0; j < numApplied; j++)
            ab.append(false);
        result->append("applied", numApplied);
        result->append("code", ex.code());
        result->append("codeName", ErrorCodes::errorString(ex.code()));
        result->append("errmsg", ex.what());
        result->append("results", ab.arr());
        return Status(ex.code(), ex.what());
    }

    return Status::OK();
}

// static
MultiApplier::Operations ApplyOps::extractOperations(const OplogEntry& applyOpsOplogEntry) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "ApplyOps::extractOperations(): not a command: "
                          << redact(applyOpsOplogEntry.toBSON()),
            applyOpsOplogEntry.isCommand());

    uassert(ErrorCodes::CommandNotSupported,
            str::stream() << "ApplyOps::extractOperations(): not applyOps command: "
                          << redact(applyOpsOplogEntry.toBSON()),
            OplogEntry::CommandType::kApplyOps == applyOpsOplogEntry.getCommandType());

    auto cmdObj = applyOpsOplogEntry.getOperationToApply();
    auto operationDocs = cmdObj.firstElement().Obj();

    if (operationDocs.isEmpty()) {
        return {};
    }

    MultiApplier::Operations operations;

    auto topLevelDoc = applyOpsOplogEntry.toBSON();
    for (const auto& elem : operationDocs) {
        auto operationDoc = elem.Obj();
        BSONObjBuilder builder(operationDoc);
        builder.appendElementsUnique(topLevelDoc);
        auto operation = builder.obj();
        operations.emplace_back(operation);
    }

    return operations;
}

}  // namespace repl
}  // namespace mongo
