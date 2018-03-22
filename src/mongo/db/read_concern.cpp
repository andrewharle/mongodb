/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/read_concern.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// This is a special flag that allows for testing of snapshot behavior by skipping the replication
// related checks and isolating the storage/query side of snapshotting.
bool testingSnapshotBehaviorInIsolation = false;
ExportedServerParameter<bool, ServerParameterType::kStartupOnly> TestingSnapshotBehaviorInIsolation(
    ServerParameterSet::getGlobal(),
    "testingSnapshotBehaviorInIsolation",
    &testingSnapshotBehaviorInIsolation);

}  // namespace

StatusWith<repl::ReadConcernArgs> extractReadConcern(OperationContext* txn,
                                                     const BSONObj& cmdObj,
                                                     bool supportsReadConcern) {
    repl::ReadConcernArgs readConcernArgs;

    auto readConcernParseStatus = readConcernArgs.initialize(cmdObj);
    if (!readConcernParseStatus.isOK()) {
        return readConcernParseStatus;
    }

    if (!supportsReadConcern && !readConcernArgs.isEmpty()) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Command does not support read concern"};
    }

    return readConcernArgs;
}

Status waitForReadConcern(OperationContext* txn, const repl::ReadConcernArgs& readConcernArgs) {
    repl::ReplicationCoordinator* const replCoord = repl::ReplicationCoordinator::get(txn);

    if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kLinearizableReadConcern) {
        if (replCoord->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet) {
            // For master/slave and standalone nodes, Linearizable Read is not supported.
            return {ErrorCodes::NotAReplicaSet,
                    "node needs to be a replica set member to use read concern"};
        }

        // Replica sets running pv0 do not support linearizable read concern until further testing
        // is completed (SERVER-27025).
        if (!replCoord->isV1ElectionProtocol()) {
            return {
                ErrorCodes::IncompatibleElectionProtocol,
                "Replica sets running protocol version 0 do not support readConcern: linearizable"};
        }

        if (!readConcernArgs.getOpTime().isNull()) {
            return {ErrorCodes::FailedToParse,
                    "afterOpTime not compatible with linearizable read concern"};
        }

        if (!replCoord->getMemberState().primary()) {
            return {ErrorCodes::NotMaster,
                    "cannot satisfy linearizable read concern on non-primary node"};
        }
    }

    // Skip waiting for the OpTime when testing snapshot behavior
    if (!testingSnapshotBehaviorInIsolation && !readConcernArgs.isEmpty()) {
        Status status = replCoord->waitUntilOpTimeForRead(txn, readConcernArgs);
        if (!status.isOK()) {
            return status;
        }
    }

    if ((replCoord->getReplicationMode() == repl::ReplicationCoordinator::Mode::modeReplSet ||
         testingSnapshotBehaviorInIsolation) &&
        readConcernArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern) {
        // ReadConcern Majority is not supported in ProtocolVersion 0.
        if (!testingSnapshotBehaviorInIsolation && !replCoord->isV1ElectionProtocol()) {
            return {ErrorCodes::ReadConcernMajorityNotEnabled,
                    str::stream() << "Replica sets running protocol version 0 do not support "
                                     "readConcern: majority"};
        }

        const int debugLevel = serverGlobalParams.clusterRole == ClusterRole::ConfigServer ? 1 : 2;

        LOG(debugLevel) << "Waiting for 'committed' snapshot to be available for reading: "
                        << readConcernArgs;

        Status status = txn->recoveryUnit()->setReadFromMajorityCommittedSnapshot();

        // Wait until a snapshot is available.
        while (status == ErrorCodes::ReadConcernMajorityNotAvailableYet) {
            LOG(debugLevel) << "Snapshot not available yet.";
            replCoord->waitUntilSnapshotCommitted(txn, SnapshotName::min());
            status = txn->recoveryUnit()->setReadFromMajorityCommittedSnapshot();
        }

        if (!status.isOK()) {
            return status;
        }

        LOG(debugLevel) << "Using 'committed' snapshot: " << CurOp::get(txn)->query();
    }

    return Status::OK();
}

Status waitForLinearizableReadConcern(OperationContext* txn) {

    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(txn->getClient()->getServiceContext());

    {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock lk(txn->lockState(), "local", MODE_IX);
        Lock::CollectionLock lock(txn->lockState(), "local.oplog.rs", MODE_IX);

        if (!replCoord->canAcceptWritesForDatabase("admin")) {
            return {ErrorCodes::NotMaster,
                    "No longer primary when waiting for linearizable read concern"};
        }

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {

            WriteUnitOfWork uow(txn);
            txn->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                txn,
                BSON("msg"
                     << "linearizable read"));
            uow.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
            txn, "waitForLinearizableReadConcern", "local.rs.oplog");
    }
    WriteConcernOptions wc = WriteConcernOptions(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, 0);

    repl::OpTime lastOpApplied = repl::ReplClientInfo::forClient(txn->getClient()).getLastOp();
    auto awaitReplResult = replCoord->awaitReplication(txn, lastOpApplied, wc);
    if (awaitReplResult.status == ErrorCodes::WriteConcernFailed) {
        return Status(ErrorCodes::LinearizableReadConcernError,
                      "Failed to confirm that read was linearizable.");
    }
    return awaitReplResult.status;
}

}  // namespace mongo
