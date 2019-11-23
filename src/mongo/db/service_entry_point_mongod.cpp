
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

#include "mongo/db/service_entry_point_mongod.h"

#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/concurrency/global_lock_acquisition_tracker.h"
#include "mongo/db/curop.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/implicit_create_collection.h"
#include "mongo/db/s/scoped_operation_completion_sharding_actions.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_config_optime_gossip.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_entry_point_common.h"
#include "mongo/logger/redaction.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {

class ServiceEntryPointMongod::Hooks final : public ServiceEntryPointCommon::Hooks {
public:
    bool lockedForWriting() const override {
        return mongo::lockedForWriting();
    }

    void waitForReadConcern(OperationContext* opCtx,
                            const CommandInvocation* invocation,
                            const OpMsgRequest& request) const override {
        Status rcStatus = mongo::waitForReadConcern(
            opCtx, repl::ReadConcernArgs::get(opCtx), invocation->allowsAfterClusterTime());

        if (!rcStatus.isOK()) {
            if (ErrorCodes::isExceededTimeLimitError(rcStatus.code())) {
                const int debugLevel =
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer ? 0 : 2;
                LOG(debugLevel) << "Command on database " << request.getDatabase()
                                << " timed out waiting for read concern to be satisfied. Command: "
                                << redact(ServiceEntryPointCommon::getRedactedCopyForLogging(
                                       invocation->definition(), request.body))
                                << ". Info: " << redact(rcStatus);
            }

            uassertStatusOK(rcStatus);
        }
    }

    void waitForWriteConcern(OperationContext* opCtx,
                             const CommandInvocation* invocation,
                             const repl::OpTime& lastOpBeforeRun,
                             BSONObjBuilder& commandResponseBuilder) const override {
        auto lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        // Ensures that if we tried to do a write, we wait for write concern, even if that write was
        // a noop.
        if ((lastOpAfterRun == lastOpBeforeRun) &&
            GlobalLockAcquisitionTracker::get(opCtx).getGlobalExclusiveLockTaken()) {
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
            lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        }

        WriteConcernResult res;
        auto waitForWCStatus =
            mongo::waitForWriteConcern(opCtx, lastOpAfterRun, opCtx->getWriteConcern(), &res);

        CommandHelpers::appendCommandWCStatus(commandResponseBuilder, waitForWCStatus, res);

        // SERVER-22421: This code is to ensure error response backwards compatibility with the
        // user management commands. This can be removed in 3.6.
        if (!waitForWCStatus.isOK() && invocation->definition()->isUserManagementCommand()) {
            BSONObj temp = commandResponseBuilder.asTempObj().copy();
            commandResponseBuilder.resetToEmpty();
            CommandHelpers::appendCommandStatusNoThrow(commandResponseBuilder, waitForWCStatus);
            commandResponseBuilder.appendElementsUnique(temp);
        }
    }

    void waitForLinearizableReadConcern(OperationContext* opCtx) const override {
        // When a linearizable read command is passed in, check to make sure we're reading
        // from the primary.
        if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
            repl::ReadConcernLevel::kLinearizableReadConcern) {
            uassertStatusOK(mongo::waitForLinearizableReadConcern(opCtx));
        }
    }

    void uassertCommandDoesNotSpecifyWriteConcern(const BSONObj& cmd) const override {
        if (commandSpecifiesWriteConcern(cmd)) {
            uasserted(ErrorCodes::InvalidOptions, "Command does not support writeConcern");
        }
    }

    void attachCurOpErrInfo(OperationContext* opCtx, const BSONObj& replyObj) const override {
        CurOp::get(opCtx)->debug().errInfo = getStatusFromCommandResult(replyObj);
    }

    void handleException(const DBException& e, OperationContext* opCtx) const override {
        // If we got a stale config, wait in case the operation is stuck in a critical section
        if (auto sce = e.extraInfo<StaleConfigInfo>()) {
            if (!opCtx->getClient()->isInDirectClient()) {
                // We already have the StaleConfig exception, so just swallow any errors due to
                // refresh
                onShardVersionMismatchNoExcept(opCtx, sce->getNss(), sce->getVersionReceived())
                    .ignore();
            }
        } else if (auto sce = e.extraInfo<StaleDbRoutingVersion>()) {
            if (!opCtx->getClient()->isInDirectClient()) {
                onDbVersionMismatchNoExcept(
                    opCtx, sce->getDb(), sce->getVersionReceived(), sce->getVersionWanted())
                    .ignore();
            }
        } else if (auto cannotImplicitCreateCollInfo =
                       e.extraInfo<CannotImplicitlyCreateCollectionInfo>()) {
            if (ShardingState::get(opCtx)->enabled()) {
                onCannotImplicitlyCreateCollection(opCtx, cannotImplicitCreateCollInfo->getNss())
                    .ignore();
            }
        }
    }

    void advanceConfigOpTimeFromRequestMetadata(OperationContext* opCtx) const override {
        // Handle config optime information that may have been sent along with the command.
        rpc::advanceConfigOpTimeFromRequestMetadata(opCtx);
    }

    std::unique_ptr<PolymorphicScoped> scopedOperationCompletionShardingActions(
        OperationContext* opCtx) const override {
        return std::make_unique<ScopedOperationCompletionShardingActions>(opCtx);
    }
};

DbResponse ServiceEntryPointMongod::handleRequest(OperationContext* opCtx, const Message& m) {
    return ServiceEntryPointCommon::handleRequest(opCtx, m, Hooks{});
}

}  // namespace mongo
