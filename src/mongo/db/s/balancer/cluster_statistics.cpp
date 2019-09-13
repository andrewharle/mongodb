
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

#include "mongo/db/s/balancer/cluster_statistics.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

ClusterStatistics::ClusterStatistics() = default;

ClusterStatistics::~ClusterStatistics() = default;

ClusterStatistics::ShardStatistics::ShardStatistics(ShardId inShardId,
                                                    uint64_t inMaxSizeMB,
                                                    uint64_t inCurrSizeMB,
                                                    bool inIsDraining,
                                                    std::set<std::string> inShardTags,
                                                    std::string inMongoVersion)
    : shardId(std::move(inShardId)),
      maxSizeMB(inMaxSizeMB),
      currSizeMB(inCurrSizeMB),
      isDraining(inIsDraining),
      shardTags(std::move(inShardTags)),
      mongoVersion(std::move(inMongoVersion)) {}

bool ClusterStatistics::ShardStatistics::isSizeMaxed() const {
    if (!maxSizeMB || !currSizeMB) {
        return false;
    }

    return currSizeMB >= maxSizeMB;
}

BSONObj ClusterStatistics::ShardStatistics::toBSON() const {
    BSONObjBuilder builder;
    builder.append("id", shardId.toString());
    builder.append("maxSizeMB", static_cast<long long>(maxSizeMB));
    builder.append("currSizeMB", static_cast<long long>(currSizeMB));
    builder.append("draining", isDraining);
    if (!shardTags.empty()) {
        BSONArrayBuilder arrayBuilder(builder.subarrayStart("tags"));
        arrayBuilder.append(shardTags);
    }

    builder.append("version", mongoVersion);
    return builder.obj();
}

}  // namespace mongo
