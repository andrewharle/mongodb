
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot_manager.h"

namespace mongo {
class CmdMakeSnapshot final : public BasicCommand {
public:
    CmdMakeSnapshot() : BasicCommand("makeSnapshot") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }

    // No auth needed because it only works when enabled via command line.
    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return Status::OK();
    }

    std::string help() const override {
        return "Creates a new named snapshot";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auto snapshotManager = getGlobalServiceContext()->getStorageEngine()->getSnapshotManager();
        if (!snapshotManager) {
            uasserted(ErrorCodes::CommandNotSupported, "");
        }

        Lock::GlobalLock lk(opCtx, MODE_IX);

        auto name = LogicalClock::getClusterTimeForReplicaSet(opCtx).asTimestamp();
        result.append("name", static_cast<long long>(name.asULL()));

        return true;
    }
};
MONGO_REGISTER_TEST_COMMAND(CmdMakeSnapshot);

class CmdSetCommittedSnapshot final : public BasicCommand {
public:
    CmdSetCommittedSnapshot() : BasicCommand("setCommittedSnapshot") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }

    // No auth needed because it only works when enabled via command line.
    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return Status::OK();
    }

    std::string help() const override {
        return "Sets the snapshot for {readConcern: {level: 'majority'}}";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auto snapshotManager = getGlobalServiceContext()->getStorageEngine()->getSnapshotManager();
        if (!snapshotManager) {
            uasserted(ErrorCodes::CommandNotSupported, "");
        }

        Lock::GlobalLock lk(opCtx, MODE_IX);
        auto timestamp = Timestamp(cmdObj.firstElement().Long());
        snapshotManager->setCommittedSnapshot(timestamp);
        return true;
    }
};
MONGO_REGISTER_TEST_COMMAND(CmdSetCommittedSnapshot);
}
