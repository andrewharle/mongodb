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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/merge_chunk_request_type.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

using std::string;

/**
 * Internal sharding command run on config servers to merge a set of chunks.
 *
 * Format:
 * {
 *   _configsvrCommitChunkMerge: <string namespace>,
 *   collEpoch: <OID epoch>,
 *   chunkBoundaries: [
 *      <BSONObj key1>,
 *      <BSONObj key2>,
 *      ...
 *   ],
 *   shard: <string shard>,
 *   writeConcern: <BSONObj>
 * }
 */
class ConfigSvrMergeChunkCommand : public Command {
public:
    ConfigSvrMergeChunkCommand() : Command("_configsvrCommitChunkMerge") {}

    void help(std::stringstream& help) const override {
        help << "Internal command, which is sent by a shard to the sharding config server. Do "
                "not call directly. Receives, validates, and processes a MergeChunkRequest";
    }

    bool slaveOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const std::string& dbName,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
            uasserted(ErrorCodes::IllegalOperation,
                      "_configsvrCommitChunkMerge can only be run on config servers");
        }

        auto parsedRequest = uassertStatusOK(MergeChunkRequest::parseFromConfigCommand(cmdObj));

        Status mergeChunkResult =
            Grid::get(txn)->catalogManager()->commitChunkMerge(txn,
                                                               parsedRequest.getNamespace(),
                                                               parsedRequest.getEpoch(),
                                                               parsedRequest.getChunkBoundaries(),
                                                               parsedRequest.getShardName());

        if (!mergeChunkResult.isOK()) {
            return appendCommandStatus(result, mergeChunkResult);
        }

        return true;
    }
} configsvrMergeChunkCmd;
}  // namespace
}  // namespace mongo
