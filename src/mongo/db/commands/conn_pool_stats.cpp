
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

#include <string>
#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

class PoolStats final : public BasicCommand {
public:
    PoolStats() : BasicCommand("connPoolStats") {}

    std::string help() const override {
        return "stats about connections between servers in a replica set or sharded cluster.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::connPoolStats);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& db,
             const mongo::BSONObj& cmdObj,
             mongo::BSONObjBuilder& result) override {
        executor::ConnectionPoolStats stats{};

        // Global connection pool connections.
        globalConnPool.appendConnectionStats(&stats);
        result.appendNumber("numClientConnections", DBClientConnection::getNumConnections());
        result.appendNumber("numAScopedConnections", AScopedConnection::getNumConnections());

        // Replication connections, if we have any
        {
            auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
            if (replCoord && replCoord->isReplEnabled()) {
                replCoord->appendConnectionStats(&stats);
            }
        }

        // Sharding connections, if we have any
        {
            auto const grid = Grid::get(opCtx);
            if (grid->getExecutorPool()) {
                grid->getExecutorPool()->appendConnectionStats(&stats);
            }

            auto const customConnPoolStatsFn = grid->getCustomConnectionPoolStatsFn();
            if (customConnPoolStatsFn) {
                customConnPoolStatsFn(&stats);
            }
        }

        // Output to a BSON object.
        stats.appendToBSON(result);

        // Always report all replica sets being tracked.
        globalRSMonitorManager.report(&result);

        return true;
    }

} poolStatsCmd;

class ShardedPoolStats final : public BasicCommand {
public:
    ShardedPoolStats() : BasicCommand("shardConnPoolStats") {}

    std::string help() const override {
        return "stats about the shard connection pool";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    /**
     * Requires the same privileges as the connPoolStats command.
     */
    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::connPoolStats);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const mongo::BSONObj& cmdObj,
             mongo::BSONObjBuilder& result) override {
        // Connection information
        executor::ConnectionPoolStats stats{};
        shardConnectionPool.appendConnectionStats(&stats);
        stats.appendToBSON(result);

        // Thread connection information
        ShardConnection::reportActiveClientConnections(&result);
        return true;
    }

} shardedPoolStatsCmd;

}  // namespace
}  // namespace mongo
