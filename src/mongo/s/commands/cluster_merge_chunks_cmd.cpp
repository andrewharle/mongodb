/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/grid.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::stringstream;
using std::vector;

namespace {

/**
 * Mongos-side command for merging chunks, passes command to appropriate shard.
 */
class ClusterMergeChunksCommand : public Command {
public:
    ClusterMergeChunksCommand() : Command("mergeChunks") {}

    void help(stringstream& h) const override {
        h << "Merge Chunks command\n"
          << "usage: { mergeChunks : <ns>, bounds : [ <min key>, <max key> ] }";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::splitChunk)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool adminOnly() const override {
        return true;
    }

    bool slaveOk() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    // Required
    static BSONField<string> nsField;
    static BSONField<vector<BSONObj>> boundsField;

    // Used to send sharding state
    static BSONField<string> shardNameField;
    static BSONField<string> configField;


    bool run(OperationContext* opCtx,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));

        auto routingInfo = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                         nss));
        const auto cm = routingInfo.cm();

        vector<BSONObj> bounds;
        if (!FieldParser::extract(cmdObj, boundsField, &bounds, &errmsg)) {
            return false;
        }

        if (bounds.size() == 0) {
            errmsg = "no bounds were specified";
            return false;
        }

        if (bounds.size() != 2) {
            errmsg = "only a min and max bound may be specified";
            return false;
        }

        BSONObj minKey = bounds[0];
        BSONObj maxKey = bounds[1];

        if (minKey.isEmpty()) {
            errmsg = "no min key specified";
            return false;
        }

        if (maxKey.isEmpty()) {
            errmsg = "no max key specified";
            return false;
        }

        if (!cm->getShardKeyPattern().isShardKey(minKey) ||
            !cm->getShardKeyPattern().isShardKey(maxKey)) {
            errmsg = stream() << "shard key bounds "
                              << "[" << minKey << "," << maxKey << ")"
                              << " are not valid for shard key pattern "
                              << cm->getShardKeyPattern().toBSON();
            return false;
        }

        minKey = cm->getShardKeyPattern().normalizeShardKey(minKey);
        maxKey = cm->getShardKeyPattern().normalizeShardKey(maxKey);

        shared_ptr<Chunk> firstChunk = cm->findIntersectingChunkWithSimpleCollation(minKey);

        BSONObjBuilder remoteCmdObjB;
        remoteCmdObjB.append(cmdObj[ClusterMergeChunksCommand::nsField()]);
        remoteCmdObjB.append(cmdObj[ClusterMergeChunksCommand::boundsField()]);
        remoteCmdObjB.append(
            ClusterMergeChunksCommand::configField(),
            Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString().toString());
        remoteCmdObjB.append(ClusterMergeChunksCommand::shardNameField(),
                             firstChunk->getShardId().toString());

        BSONObj remoteResult;

        // Throws, but handled at level above.  Don't want to rewrap to preserve exception
        // formatting.
        const auto shardStatus =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, firstChunk->getShardId());
        if (!shardStatus.isOK()) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::ShardNotFound,
                       str::stream() << "Can't find shard for chunk: " << firstChunk->toString()));
        }

        ShardConnection conn(shardStatus.getValue()->getConnString(), "");
        bool ok = conn->runCommand("admin", remoteCmdObjB.obj(), remoteResult);
        conn.done();

        Grid::get(opCtx)->catalogCache()->onStaleConfigError(std::move(routingInfo));

        result.appendElements(remoteResult);
        return ok;
    }

} clusterMergeChunksCommand;

BSONField<string> ClusterMergeChunksCommand::nsField("mergeChunks");
BSONField<vector<BSONObj>> ClusterMergeChunksCommand::boundsField("bounds");

BSONField<string> ClusterMergeChunksCommand::configField("config");
BSONField<string> ClusterMergeChunksCommand::shardNameField("shardName");

}  // namespace
}  // namespace mongo
