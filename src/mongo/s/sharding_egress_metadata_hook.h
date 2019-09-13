
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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/repl/optime.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/client/shard.h"

namespace mongo {
namespace rpc {

/**
 * Hooks for handling configsvr optime, client metadata and auth metadata for sharding.
 */
class ShardingEgressMetadataHook : public rpc::EgressMetadataHook {
public:
    ShardingEgressMetadataHook(ServiceContext* serviceContext);
    virtual ~ShardingEgressMetadataHook() = default;

    Status readReplyMetadata(OperationContext* opCtx,
                             StringData replySource,
                             const BSONObj& metadataObj) override;
    Status writeRequestMetadata(OperationContext* opCtx, BSONObjBuilder* metadataBob) override;

protected:
    /**
     * On mongod this is a no-op.
     * On mongos it looks for $gleStats in a command's reply metadata, and fills in the
     * ClusterLastErrorInfo for this thread's associated Client with the data, if found.
     * This data will be used by subsequent GLE calls, to ensure we look for the correct write on
     * the correct PRIMARY.
     */
    virtual void _saveGLEStats(const BSONObj& metadata, StringData hostString) = 0;

    /**
     * Called by writeRequestMetadata() to find the config server optime that should be sent as part
     * of the ConfigServerMetadata.
     */
    virtual repl::OpTime _getConfigServerOpTime() = 0;

    /**
     * On config servers this is a no-op.
     * On shards and mongoses this advances the Grid's stored config server optime based on the
     * metadata in the response object from running a command.
     */
    virtual Status _advanceConfigOptimeFromShard(ShardId shardId, const BSONObj& metadataObj);

    ServiceContext* const _serviceContext;
};

}  // namespace rpc
}  // namespace mongo
