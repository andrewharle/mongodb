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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/data_replicator_external_state_mock.h"

#include "mongo/db/repl/oplog_buffer_blocking_queue.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace repl {

DataReplicatorExternalStateMock::DataReplicatorExternalStateMock()
    : multiApplyFn([](OperationContext*,
                      const MultiApplier::Operations& ops,
                      MultiApplier::ApplyOperationFn) { return ops.back().getOpTime(); }) {}

executor::TaskExecutor* DataReplicatorExternalStateMock::getTaskExecutor() const {
    return taskExecutor;
}

OldThreadPool* DataReplicatorExternalStateMock::getDbWorkThreadPool() const {
    return dbWorkThreadPool;
}

OpTimeWithTerm DataReplicatorExternalStateMock::getCurrentTermAndLastCommittedOpTime() {
    return {currentTerm, lastCommittedOpTime};
}

void DataReplicatorExternalStateMock::processMetadata(const rpc::ReplSetMetadata& metadata) {
    metadataProcessed = metadata;
}

bool DataReplicatorExternalStateMock::shouldStopFetching(const HostAndPort& source,
                                                         const rpc::ReplSetMetadata& metadata) {
    lastSyncSourceChecked = source;
    syncSourceLastOpTime = metadata.getLastOpVisible();
    syncSourceHasSyncSource = metadata.getSyncSourceIndex() != -1;
    return shouldStopFetchingResult;
}

std::unique_ptr<OplogBuffer> DataReplicatorExternalStateMock::makeInitialSyncOplogBuffer(
    OperationContext* txn) const {
    return stdx::make_unique<OplogBufferBlockingQueue>();
}

std::unique_ptr<OplogBuffer> DataReplicatorExternalStateMock::makeSteadyStateOplogBuffer(
    OperationContext* txn) const {
    return stdx::make_unique<OplogBufferBlockingQueue>();
}

StatusWith<ReplicaSetConfig> DataReplicatorExternalStateMock::getCurrentConfig() const {
    return replSetConfig;
}

StatusWith<OpTime> DataReplicatorExternalStateMock::_multiApply(
    OperationContext* txn,
    MultiApplier::Operations ops,
    MultiApplier::ApplyOperationFn applyOperation) {
    return multiApplyFn(txn, std::move(ops), applyOperation);
}

Status DataReplicatorExternalStateMock::_multiSyncApply(MultiApplier::OperationPtrs* ops) {
    return Status::OK();
}

Status DataReplicatorExternalStateMock::_multiInitialSyncApply(MultiApplier::OperationPtrs* ops,
                                                               const HostAndPort& source,
                                                               AtomicUInt32* fetchCount) {
    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
