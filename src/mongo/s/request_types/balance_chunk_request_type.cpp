
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

#include "mongo/s/request_types/balance_chunk_request_type.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/request_types/balance_chunk_request_type.h"

namespace mongo {
namespace {

const char kConfigSvrMoveChunk[] = "_configsvrMoveChunk";
const char kMaxChunkSizeBytes[] = "maxChunkSizeBytes";
const char kToShardId[] = "toShard";
const char kSecondaryThrottle[] = "secondaryThrottle";
const char kWaitForDelete[] = "waitForDelete";
const char kWaitForDeleteDeprecated[] = "_waitForDelete";

const WriteConcernOptions kMajorityWriteConcernNoTimeout(WriteConcernOptions::kMajority,
                                                         WriteConcernOptions::SyncMode::UNSET,
                                                         Seconds(15));

}  // namespace

BalanceChunkRequest::BalanceChunkRequest(ChunkType chunk,
                                         MigrationSecondaryThrottleOptions secondaryThrottle)
    : _chunk(std::move(chunk)), _secondaryThrottle(std::move(secondaryThrottle)) {}

StatusWith<BalanceChunkRequest> BalanceChunkRequest::parseFromConfigCommand(const BSONObj& obj) {
    auto chunkStatus = ChunkType::fromConfigBSON(obj);
    if (!chunkStatus.isOK()) {
        return chunkStatus.getStatus();
    }

    // The secondary throttle options being sent to the config server are contained within a
    // sub-object on the request because they contain the writeConcern field, which when sent to the
    // config server gets checked for only being w:1 or w:majoirty.
    BSONObj secondaryThrottleObj;

    {
        BSONElement secondaryThrottleElement;
        auto secondaryThrottleElementStatus =
            bsonExtractTypedField(obj, kSecondaryThrottle, Object, &secondaryThrottleElement);

        if (secondaryThrottleElementStatus.isOK()) {
            secondaryThrottleObj = secondaryThrottleElement.Obj();
        } else if (secondaryThrottleElementStatus != ErrorCodes::NoSuchKey) {
            return secondaryThrottleElementStatus;
        }
    }

    auto secondaryThrottleStatus =
        MigrationSecondaryThrottleOptions::createFromCommand(secondaryThrottleObj);
    if (!secondaryThrottleStatus.isOK()) {
        return secondaryThrottleStatus.getStatus();
    }

    BalanceChunkRequest request(std::move(chunkStatus.getValue()),
                                std::move(secondaryThrottleStatus.getValue()));

    {
        Status status =
            bsonExtractBooleanFieldWithDefault(obj, kWaitForDelete, false, &request._waitForDelete);
        if (!status.isOK()) {
            return status;
        }
    }

    // Check for the deprecated name '_waitForDelete' 'waitForDelete' was false.
    if (!request._waitForDelete) {
        Status status = bsonExtractBooleanFieldWithDefault(
            obj, kWaitForDeleteDeprecated, false, &request._waitForDelete);
        if (!status.isOK()) {
            return status;
        }
    }

    {
        long long maxChunkSizeBytes;
        Status status =
            bsonExtractIntegerFieldWithDefault(obj, kMaxChunkSizeBytes, 0, &maxChunkSizeBytes);
        if (!status.isOK()) {
            return status;
        }

        request._maxChunkSizeBytes = static_cast<int64_t>(maxChunkSizeBytes);
    }

    {
        std::string toShardId;
        Status status = bsonExtractStringField(obj, kToShardId, &toShardId);
        if (status.isOK()) {
            if (toShardId.empty()) {
                return {ErrorCodes::BadValue, "To shard cannot be empty"};
            }

            request._toShardId = std::move(toShardId);
        } else if (status != ErrorCodes::NoSuchKey) {
            return status;
        }
    }

    return request;
}

BSONObj BalanceChunkRequest::serializeToMoveCommandForConfig(
    const ChunkType& chunk,
    const ShardId& newShardId,
    int64_t maxChunkSizeBytes,
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    bool waitForDelete) {
    invariant(chunk.validate());

    BSONObjBuilder cmdBuilder;
    cmdBuilder.append(kConfigSvrMoveChunk, 1);
    cmdBuilder.appendElements(chunk.toConfigBSON());
    cmdBuilder.append(kToShardId, newShardId.toString());
    cmdBuilder.append(kMaxChunkSizeBytes, static_cast<long long>(maxChunkSizeBytes));
    {
        BSONObjBuilder secondaryThrottleBuilder(cmdBuilder.subobjStart(kSecondaryThrottle));
        secondaryThrottle.append(&secondaryThrottleBuilder);
        secondaryThrottleBuilder.doneFast();
    }
    cmdBuilder.append(kWaitForDelete, waitForDelete);
    cmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                      kMajorityWriteConcernNoTimeout.toBSON());

    return cmdBuilder.obj();
}

BSONObj BalanceChunkRequest::serializeToRebalanceCommandForConfig(const ChunkType& chunk) {
    invariant(chunk.validate());

    BSONObjBuilder cmdBuilder;
    cmdBuilder.append(kConfigSvrMoveChunk, 1);
    cmdBuilder.appendElements(chunk.toConfigBSON());
    cmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                      kMajorityWriteConcernNoTimeout.toBSON());

    return cmdBuilder.obj();
}

}  // namespace mongo
