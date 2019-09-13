
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

#include "mongo/s/request_types/split_chunk_request_type.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"

namespace mongo {

using std::string;
using std::vector;

namespace {

const char kConfigsvrSplitChunk[] = "_configsvrCommitChunkSplit";
const char kCollEpoch[] = "collEpoch";
const char kSplitPoints[] = "splitPoints";
const char kShardName[] = "shard";

}  // unnamed namespace

SplitChunkRequest::SplitChunkRequest(NamespaceString nss,
                                     string shardName,
                                     OID epoch,
                                     ChunkRange chunkRange,
                                     vector<BSONObj> splitPoints)
    : _nss(std::move(nss)),
      _epoch(std::move(epoch)),
      _chunkRange(std::move(chunkRange)),
      _splitPoints(std::move(splitPoints)),
      _shardName(std::move(shardName)) {}

StatusWith<SplitChunkRequest> SplitChunkRequest::parseFromConfigCommand(const BSONObj& cmdObj) {
    string ns;
    auto parseNamespaceStatus = bsonExtractStringField(cmdObj, kConfigsvrSplitChunk, &ns);

    if (!parseNamespaceStatus.isOK()) {
        return parseNamespaceStatus;
    }

    OID epoch;
    auto parseEpochStatus = bsonExtractOIDField(cmdObj, kCollEpoch, &epoch);

    if (!parseEpochStatus.isOK()) {
        return parseEpochStatus;
    }

    auto chunkRangeStatus = ChunkRange::fromBSON(cmdObj);

    if (!chunkRangeStatus.isOK()) {
        return chunkRangeStatus.getStatus();
    }

    vector<BSONObj> splitPoints;
    {
        BSONElement splitPointsElem;
        auto splitPointsElemStatus =
            bsonExtractTypedField(cmdObj, kSplitPoints, mongo::Array, &splitPointsElem);

        if (!splitPointsElemStatus.isOK()) {
            return splitPointsElemStatus;
        }
        BSONObjIterator it(splitPointsElem.Obj());
        while (it.more()) {
            splitPoints.push_back(it.next().Obj().getOwned());
        }
    }

    string shardName;
    auto parseShardNameStatus = bsonExtractStringField(cmdObj, kShardName, &shardName);

    if (!parseShardNameStatus.isOK()) {
        return parseShardNameStatus;
    }

    auto request = SplitChunkRequest(NamespaceString(ns),
                                     std::move(shardName),
                                     std::move(epoch),
                                     std::move(chunkRangeStatus.getValue()),
                                     std::move(splitPoints));
    Status validationStatus = request._validate();
    if (!validationStatus.isOK()) {
        return validationStatus;
    }

    return request;
}

BSONObj SplitChunkRequest::toConfigCommandBSON(const BSONObj& writeConcern) {
    BSONObjBuilder cmdBuilder;
    appendAsConfigCommand(&cmdBuilder);

    // Tack on passed-in writeConcern
    cmdBuilder.appendElements(writeConcern);

    return cmdBuilder.obj();
}

void SplitChunkRequest::appendAsConfigCommand(BSONObjBuilder* cmdBuilder) {
    cmdBuilder->append(kConfigsvrSplitChunk, _nss.ns());
    cmdBuilder->append(kCollEpoch, _epoch);
    _chunkRange.append(cmdBuilder);
    {
        BSONArrayBuilder splitPointsArray(cmdBuilder->subarrayStart(kSplitPoints));
        for (const auto& splitPoint : _splitPoints) {
            splitPointsArray.append(splitPoint);
        }
    }
    cmdBuilder->append(kShardName, _shardName);
}

const NamespaceString& SplitChunkRequest::getNamespace() const {
    return _nss;
}

const OID& SplitChunkRequest::getEpoch() const {
    return _epoch;
}

const ChunkRange& SplitChunkRequest::getChunkRange() const {
    return _chunkRange;
}

const vector<BSONObj>& SplitChunkRequest::getSplitPoints() const {
    return _splitPoints;
}

const string& SplitChunkRequest::getShardName() const {
    return _shardName;
}

Status SplitChunkRequest::_validate() {
    if (!getNamespace().isValid()) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid namespace '" << _nss.ns()
                                    << "' specified for request");
    }

    if (getSplitPoints().empty()) {
        return Status(ErrorCodes::InvalidOptions, "need to provide the split points");
    }

    return Status::OK();
}

}  // namespace mongo
