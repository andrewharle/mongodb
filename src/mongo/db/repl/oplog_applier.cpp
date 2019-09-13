
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_applier.h"

#include "mongo/db/repl/sync_tail.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

using CallbackArgs = executor::TaskExecutor::CallbackArgs;

OplogApplier::OplogApplier(executor::TaskExecutor* executor,
                           OplogBuffer* oplogBuffer,
                           Observer* observer,
                           ReplicationCoordinator* replCoord,
                           ReplicationConsistencyMarkers* consistencyMarkers,
                           StorageInterface* storageInterface,
                           const OplogApplier::Options& options,
                           ThreadPool* writerPool)
    : _executor(executor),
      _oplogBuffer(oplogBuffer),
      _observer(observer),
      _replCoord(replCoord),
      _consistencyMarkers(consistencyMarkers),
      _storageInterface(storageInterface),
      _options(options),
      _syncTail(std::make_unique<SyncTail>(
          _observer, _consistencyMarkers, _storageInterface, multiSyncApply, writerPool, options)) {
    invariant(!options.relaxUniqueIndexConstraints);
}

Future<void> OplogApplier::startup() {
    auto pf = makePromiseFuture<void>();
    auto callback =
        [ this, promise = pf.promise.share() ](const CallbackArgs& args) mutable noexcept {
        invariant(args.status);
        log() << "Starting oplog application";
        _syncTail->oplogApplication(_oplogBuffer, _replCoord);
        log() << "Finished oplog application";
        promise.setWith([] {});
    };
    invariant(_executor->scheduleWork(callback).getStatus());
    return std::move(pf.future);
}

void OplogApplier::shutdown() {
    _syncTail->shutdown();
}

/**
 * Pushes operations read from sync source into oplog buffer.
 */
void OplogApplier::enqueue(const Operations& operations) {}

StatusWith<OplogApplier::Operations> OplogApplier::getNextApplierBatch(
    OperationContext* opCtx, const BatchLimits& batchLimits) {
    if (batchLimits.ops == 0) {
        return Status(ErrorCodes::InvalidOptions, "Batch size must be greater than 0.");
    }

    std::uint32_t totalBytes = 0;
    Operations ops;
    BSONObj op;
    while (_oplogBuffer->peek(opCtx, &op)) {
        auto entry = OplogEntry(op);

        // Check for oplog version change. If it is absent, its value is one.
        if (entry.getVersion() != OplogEntry::kOplogVersion) {
            std::string message = str::stream()
                << "expected oplog version " << OplogEntry::kOplogVersion << " but found version "
                << entry.getVersion() << " in oplog entry: " << redact(entry.toBSON());
            severe() << message;
            return {ErrorCodes::BadValue, message};
        }

        // Commands must be processed one at a time. The only exception to this is applyOps because
        // applyOps oplog entries are effectively containers for CRUD operations. Therefore, it is
        // safe to batch applyOps commands with CRUD operations when reading from the oplog buffer.
        if (entry.isCommand() && entry.getCommandType() != OplogEntry::CommandType::kApplyOps) {
            if (ops.empty()) {
                // Apply commands one-at-a-time.
                ops.push_back(std::move(entry));
                BSONObj opToPopAndDiscard;
                invariant(_oplogBuffer->tryPop(opCtx, &opToPopAndDiscard));
                dassert(ops.back() == OplogEntry(opToPopAndDiscard));
            }

            // Otherwise, apply what we have so far and come back for the command.
            return std::move(ops);
        }

        // Apply replication batch limits.
        if (ops.size() >= batchLimits.ops) {
            return std::move(ops);
        }

        // Never return an empty batch if there are operations left.
        if ((totalBytes + entry.getRawObjSizeBytes() >= batchLimits.bytes) && (ops.size() > 0)) {
            return std::move(ops);
        }

        // Add op to buffer.
        totalBytes += entry.getRawObjSizeBytes();
        ops.push_back(std::move(entry));
        BSONObj opToPopAndDiscard;
        invariant(_oplogBuffer->tryPop(opCtx, &opToPopAndDiscard));
        dassert(ops.back() == OplogEntry(opToPopAndDiscard));
    }
    return std::move(ops);
}

StatusWith<OpTime> OplogApplier::multiApply(OperationContext* opCtx, Operations ops) {
    _observer->onBatchBegin(ops);
    auto lastApplied = _syncTail->multiApply(opCtx, std::move(ops));
    _observer->onBatchEnd(lastApplied, {});
    return lastApplied;
}

}  // namespace repl
}  // namespace mongo
