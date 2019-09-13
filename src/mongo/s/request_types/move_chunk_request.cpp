
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

#include "mongo/s/request_types/move_chunk_request.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/logger/redaction.h"

namespace mongo {
namespace {

const char kMoveChunk[] = "moveChunk";
const char kEpoch[] = "epoch";
const char kChunkVersion[] = "chunkVersion";
const char kConfigServerConnectionString[] = "configdb";
const char kFromShardId[] = "fromShard";
const char kToShardId[] = "toShard";
const char kMaxChunkSizeBytes[] = "maxChunkSizeBytes";
const char kWaitForDelete[] = "waitForDelete";
const char kWaitForDeleteDeprecated[] = "_waitForDelete";
const char kTakeDistLock[] = "takeDistLock";  // TODO: delete in 3.8

}  // namespace

MoveChunkRequest::MoveChunkRequest(NamespaceString nss,
                                   ChunkRange range,
                                   MigrationSecondaryThrottleOptions secondaryThrottle)
    : _nss(std::move(nss)),
      _range(std::move(range)),
      _secondaryThrottle(std::move(secondaryThrottle)) {}

StatusWith<MoveChunkRequest> MoveChunkRequest::createFromCommand(NamespaceString nss,
                                                                 const BSONObj& obj) {
    auto secondaryThrottleStatus = MigrationSecondaryThrottleOptions::createFromCommand(obj);
    if (!secondaryThrottleStatus.isOK()) {
        return secondaryThrottleStatus.getStatus();
    }

    auto rangeStatus = ChunkRange::fromBSON(obj);
    if (!rangeStatus.isOK()) {
        return rangeStatus.getStatus();
    }

    MoveChunkRequest request(std::move(nss),
                             std::move(rangeStatus.getValue()),
                             std::move(secondaryThrottleStatus.getValue()));

    {
        std::string shardStr;
        Status status = bsonExtractStringField(obj, kFromShardId, &shardStr);
        request._fromShardId = shardStr;
        if (!status.isOK()) {
            return status;
        }
    }

    {
        std::string shardStr;
        Status status = bsonExtractStringField(obj, kToShardId, &shardStr);
        request._toShardId = shardStr;
        if (!status.isOK()) {
            return status;
        }
    }

    {
        BSONElement epochElem;
        Status status = bsonExtractTypedField(obj, kEpoch, BSONType::jstOID, &epochElem);
        if (!status.isOK())
            return status;
        request._versionEpoch = epochElem.OID();
    }

    {
        Status status =
            bsonExtractBooleanFieldWithDefault(obj, kWaitForDelete, false, &request._waitForDelete);
        if (!status.isOK()) {
            return status;
        }
    }

    // Check for the deprecated name '_waitForDelete' if 'waitForDelete' was false.
    if (!request._waitForDelete) {
        Status status = bsonExtractBooleanFieldWithDefault(
            obj, kWaitForDeleteDeprecated, false, &request._waitForDelete);
        if (!status.isOK()) {
            return status;
        }
    }

    {
        long long maxChunkSizeBytes;
        Status status = bsonExtractIntegerField(obj, kMaxChunkSizeBytes, &maxChunkSizeBytes);
        if (!status.isOK()) {
            return status;
        }

        request._maxChunkSizeBytes = static_cast<int64_t>(maxChunkSizeBytes);
    }

    {  // TODO: delete this block in 3.8
        bool takeDistLock = false;
        Status status = bsonExtractBooleanField(obj, kTakeDistLock, &takeDistLock);
        if (status.isOK() && takeDistLock) {
            return Status{ErrorCodes::IncompatibleShardingConfigVersion,
                          str::stream()
                              << "Request received from an older, incompatible mongodb version"};
        }
    }

    return request;
}

void MoveChunkRequest::appendAsCommand(BSONObjBuilder* builder,
                                       const NamespaceString& nss,
                                       ChunkVersion chunkVersion,
                                       const ConnectionString& configServerConnectionString,
                                       const ShardId& fromShardId,
                                       const ShardId& toShardId,
                                       const ChunkRange& range,
                                       int64_t maxChunkSizeBytes,
                                       const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                       bool waitForDelete) {
    invariant(builder->asTempObj().isEmpty());
    invariant(nss.isValid());

    builder->append(kMoveChunk, nss.ns());
    chunkVersion.appendToCommand(builder);  // 3.4 shard compatibility
    builder->append(kEpoch, chunkVersion.epoch());
    // config connection string is included for 3.4 shard compatibility
    builder->append(kConfigServerConnectionString, configServerConnectionString.toString());
    builder->append(kFromShardId, fromShardId.toString());
    builder->append(kToShardId, toShardId.toString());
    range.append(builder);
    builder->append(kMaxChunkSizeBytes, static_cast<long long>(maxChunkSizeBytes));
    secondaryThrottle.append(builder);
    builder->append(kWaitForDelete, waitForDelete);
    builder->append(kTakeDistLock, false);
}

bool MoveChunkRequest::operator==(const MoveChunkRequest& other) const {
    if (_nss != other._nss)
        return false;
    if (_fromShardId != other._fromShardId)
        return false;
    if (_toShardId != other._toShardId)
        return false;
    if (_range != other._range)
        return false;
    if (_waitForDelete != other._waitForDelete)
        return false;
    return true;
}

bool MoveChunkRequest::operator!=(const MoveChunkRequest& other) const {
    return !(*this == other);
}

std::string MoveChunkRequest::toString() const {
    std::stringstream ss;
    ss << "ns: " << getNss().ns() << ", " << redact(ChunkRange(getMinKey(), getMaxKey()).toString())
       << ", fromShard: " << getFromShardId() << ", toShard: " << getToShardId();

    return ss.str();
}

}  // namespace mongo
