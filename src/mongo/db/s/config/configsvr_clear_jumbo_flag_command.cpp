/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/clear_jumbo_flag_gen.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

class ConfigsvrClearJumboFlagCommand final : public TypedCommand<ConfigsvrClearJumboFlagCommand> {
public:
    using Request = ConfigsvrClearJumboFlag;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrClearJumboFlag can only be run on config servers",
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
            uassert(ErrorCodes::InvalidOptions,
                    "_configsvrClearJumboFlag must be called with majority writeConcern",
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const auto catalogClient = Grid::get(opCtx)->catalogClient();

            // Acquire distlocks on the namespace's database and collection.
            DistLockManager::ScopedDistLock dbDistLock(
                uassertStatusOK(catalogClient->getDistLockManager()->lock(
                    opCtx, nss.db(), "clearJumboFlag", DistLockManager::kDefaultLockTimeout)));
            DistLockManager::ScopedDistLock collDistLock(
                uassertStatusOK(catalogClient->getDistLockManager()->lock(
                    opCtx, nss.ns(), "clearJumboFlag", DistLockManager::kDefaultLockTimeout)));

            const auto collStatus =
                catalogClient->getCollection(opCtx, nss, repl::ReadConcernLevel::kLocalReadConcern);

            uassert(ErrorCodes::NamespaceNotSharded,
                    str::stream() << "clearJumboFlag namespace " << nss.toString()
                                  << " is not sharded",
                    collStatus != ErrorCodes::NamespaceNotFound);

            const auto collType = uassertStatusOK(collStatus).value;

            uassert(ErrorCodes::StaleEpoch,
                    str::stream()
                        << "clearJumboFlag namespace "
                        << nss.toString()
                        << " has a different epoch than mongos had in its routing table cache",
                    request().getEpoch() == collType.getEpoch());

            ShardingCatalogManager::get(opCtx)->clearJumboFlag(
                opCtx,
                nss,
                request().getEpoch(),
                ChunkRange{request().getMinKey(), request().getMaxKey()});
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Clears the jumbo flag of the chunk specified.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} configsvrRefineCollectionShardKeyCmd;

}  // namespace
}  // namespace mongo
