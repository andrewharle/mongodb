
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

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/chunk_move_write_concern_options.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/move_timing_helper.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/s/request_types/move_chunk_request.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

/**
 * If the specified status is not OK logs a warning and throws a DBException corresponding to the
 * specified status.
 */
void uassertStatusOKWithWarning(const Status& status) {
    if (!status.isOK()) {
        warning() << "Chunk move failed" << causedBy(redact(status));
        uassertStatusOK(status);
    }
}

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                // Note: Even though we're setting UNSET here,
                                                // kMajority implies JOURNAL if journaling is
                                                // supported by mongod and
                                                // writeConcernMajorityJournalDefault is set to true
                                                // in the ReplSetConfig.
                                                WriteConcernOptions::SyncMode::UNSET,
                                                -1);

// Tests can pause and resume moveChunk's progress at each step by enabling/disabling each failpoint
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep1);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep2);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep3);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep4);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep5);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep6);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep7);

class MoveChunkCommand : public BasicCommand {
public:
    MoveChunkCommand() : BasicCommand("moveChunk") {}

    std::string help() const override {
        return "should not be calling this directly";
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

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto shardingState = ShardingState::get(opCtx);
        uassertStatusOK(shardingState->canAcceptShardedCommands());

        const MoveChunkRequest moveChunkRequest = uassertStatusOK(
            MoveChunkRequest::createFromCommand(NamespaceString(parseNs(dbname, cmdObj)), cmdObj));

        // Make sure we're as up-to-date as possible with shard information. This catches the case
        // where we might have changed a shard's host by removing/adding a shard with the same name.
        Grid::get(opCtx)->shardRegistry()->reload(opCtx);

        auto scopedMigration = uassertStatusOK(
            ActiveMigrationsRegistry::get(opCtx).registerDonateChunk(moveChunkRequest));

        Status status = {ErrorCodes::InternalError, "Uninitialized value"};

        // Check if there is an existing migration running and if so, join it
        if (scopedMigration.mustExecute()) {
            try {
                _runImpl(opCtx, moveChunkRequest);
                status = Status::OK();
            } catch (const DBException& e) {
                status = e.toStatus();
            } catch (const std::exception& e) {
                scopedMigration.signalComplete(
                    {ErrorCodes::InternalError,
                     str::stream() << "Severe error occurred while running moveChunk command: "
                                   << e.what()});
                throw;
            }

            scopedMigration.signalComplete(status);
        } else {
            status = scopedMigration.waitForCompletion(opCtx);
        }
        uassertStatusOK(status);

        if (moveChunkRequest.getWaitForDelete()) {
            // Ensure we capture the latest opTime in the system, since range deletion happens
            // asynchronously with a different OperationContext. This must be done after the above
            // join, because each caller must set the opTime to wait for writeConcern for on its own
            // OperationContext.
            // TODO (SERVER-30183): If this moveChunk joined an active moveChunk that did not have
            // waitForDelete=true, the captured opTime may not reflect all the deletes.
            auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
            replClient.setLastOpToSystemLastOpTime(opCtx);

            WriteConcernResult writeConcernResult;
            writeConcernResult.wTimedOut = false;
            Status majorityStatus = waitForWriteConcern(
                opCtx, replClient.getLastOp(), kMajorityWriteConcern, &writeConcernResult);
            if (!majorityStatus.isOK()) {
                if (!writeConcernResult.wTimedOut) {
                    uassertStatusOK(majorityStatus);
                }
                return false;
            }
        }

        return true;
    }

private:
    static void _runImpl(OperationContext* opCtx, const MoveChunkRequest& moveChunkRequest) {
        const auto writeConcernForRangeDeleter =
            uassertStatusOK(ChunkMoveWriteConcernOptions::getEffectiveWriteConcern(
                opCtx, moveChunkRequest.getSecondaryThrottle()));

        // Resolve the donor and recipient shards and their connection string
        auto const shardRegistry = Grid::get(opCtx)->shardRegistry();

        const auto donorConnStr =
            uassertStatusOK(shardRegistry->getShard(opCtx, moveChunkRequest.getFromShardId()))
                ->getConnString();
        const auto recipientHost = uassertStatusOK([&] {
            auto recipientShard =
                uassertStatusOK(shardRegistry->getShard(opCtx, moveChunkRequest.getToShardId()));

            return recipientShard->getTargeter()->findHostNoWait(
                ReadPreferenceSetting{ReadPreference::PrimaryOnly});
        }());

        std::string unusedErrMsg;
        MoveTimingHelper moveTimingHelper(opCtx,
                                          "from",
                                          moveChunkRequest.getNss().ns(),
                                          moveChunkRequest.getMinKey(),
                                          moveChunkRequest.getMaxKey(),
                                          6,  // Total number of steps
                                          &unusedErrMsg,
                                          moveChunkRequest.getToShardId(),
                                          moveChunkRequest.getFromShardId());

        moveTimingHelper.done(1);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep1);

        MigrationSourceManager migrationSourceManager(
            opCtx, moveChunkRequest, donorConnStr, recipientHost);

        moveTimingHelper.done(2);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep2);

        uassertStatusOKWithWarning(migrationSourceManager.startClone(opCtx));
        moveTimingHelper.done(3);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep3);

        uassertStatusOKWithWarning(migrationSourceManager.awaitToCatchUp(opCtx));
        moveTimingHelper.done(4);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep4);

        uassertStatusOKWithWarning(migrationSourceManager.enterCriticalSection(opCtx));
        uassertStatusOKWithWarning(migrationSourceManager.commitChunkOnRecipient(opCtx));
        moveTimingHelper.done(5);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep5);

        uassertStatusOKWithWarning(migrationSourceManager.commitChunkMetadataOnConfig(opCtx));
        moveTimingHelper.done(6);
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(moveChunkHangAtStep6);
    }

} moveChunkCmd;

}  // namespace
}  // namespace mongo
