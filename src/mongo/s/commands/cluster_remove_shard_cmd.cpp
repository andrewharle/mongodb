
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/commands.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

class RemoveShardCmd : public BasicCommand {
public:
    RemoveShardCmd() : BasicCommand("removeShard", "removeshard") {}

    std::string help() const override {
        return "remove a shard from the system.";
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

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::removeShard);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "Field '" << cmdObj.firstElement().fieldName()
                              << "' must be of type string",
                cmdObj.firstElement().type() == BSONType::String);
        const std::string target = cmdObj.firstElement().str();

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto cmdResponseStatus = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            "admin",
            CommandHelpers::appendMajorityWriteConcern(CommandHelpers::appendPassthroughFields(
                cmdObj, BSON("_configsvrRemoveShard" << target))),
            Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(cmdResponseStatus.commandStatus);

        CommandHelpers::filterCommandReplyForPassthrough(cmdResponseStatus.response, &result);

        return true;
    }

} removeShardCmd;

}  // namespace
}  // namespace mongo
