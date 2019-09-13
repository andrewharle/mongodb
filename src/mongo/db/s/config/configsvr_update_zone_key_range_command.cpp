
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/update_zone_key_range_request_type.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

using std::string;

/**
 * Internal sharding command run on config servers to add a shard to zone.
 *
 * Format:
 * {
 *   _configsvrUpdateZoneKeyRange: <string namespace>,
 *   min: <BSONObj min>,
 *   max: <BSONObj max>,
 *   zone: <string zone|null>,
 *   writeConcern: <BSONObj>
 * }
 */
class ConfigsvrUpdateZoneKeyRangeCommand : public BasicCommand {
public:
    ConfigsvrUpdateZoneKeyRangeCommand() : BasicCommand("_configsvrUpdateZoneKeyRange") {}

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Validates and assigns a new range to a zone.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& unusedDbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
            uasserted(ErrorCodes::IllegalOperation,
                      "_configsvrAssignKeyRangeToZone can only be run on config servers");
        }

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        auto parsedRequest =
            uassertStatusOK(UpdateZoneKeyRangeRequest::parseFromConfigCommand(cmdObj));

        std::string zoneName;
        if (!parsedRequest.isRemove()) {
            zoneName = parsedRequest.getZoneName();
        }

        if (parsedRequest.isRemove()) {
            uassertStatusOK(ShardingCatalogManager::get(opCtx)->removeKeyRangeFromZone(
                opCtx, parsedRequest.getNS(), parsedRequest.getRange()));
        } else {
            uassertStatusOK(ShardingCatalogManager::get(opCtx)->assignKeyRangeToZone(
                opCtx, parsedRequest.getNS(), parsedRequest.getRange(), zoneName));
        }

        return true;
    }

} configsvrUpdateZoneKeyRangeCmd;

}  // namespace
}  // namespace mongo
