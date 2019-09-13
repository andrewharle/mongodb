
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

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/config_server_op_observer.h"
#include "mongo/db/s/op_observer_sharding_impl.h"
#include "mongo/db/s/shard_server_catalog_cache_loader.h"
#include "mongo/db/s/shard_server_op_observer.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/shard_server_test_fixture.h"

namespace mongo {
namespace {

const std::string kShardName("TestShard");

/**
 * This test suite directly invokes the sharding initialization code and validates its behaviour and
 * proper state transitions.
 */
class ShardingInitializationMongoDTest : public ShardingMongodTestFixture {
protected:
    // Used to write to set up local collections before exercising server logic.
    std::unique_ptr<DBDirectClient> _dbDirectClient;

    void setUp() override {
        serverGlobalParams.clusterRole = ClusterRole::None;
        ShardingMongodTestFixture::setUp();

        // When sharding initialization is triggered, initialize sharding state as a shard server.
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;

        CatalogCacheLoader::set(getServiceContext(),
                                stdx::make_unique<ShardServerCatalogCacheLoader>(
                                    stdx::make_unique<ConfigServerCatalogCacheLoader>()));

        ShardingInitializationMongoD::get(getServiceContext())
            ->setGlobalInitMethodForTest([&](OperationContext* opCtx,
                                             const ShardIdentity& shardIdentity,
                                             StringData distLockProcessId) {
                const auto& configConnStr = shardIdentity.getConfigsvrConnectionString();

                uassertStatusOK(initializeGlobalShardingStateForMongodForTest(configConnStr));

                // Set the ConnectionString return value on the mock targeter so that later calls to
                // the
                // targeter's getConnString() return the appropriate value
                auto configTargeter = RemoteCommandTargeterMock::get(
                    shardRegistry()->getConfigShard()->getTargeter());
                configTargeter->setConnectionStringReturnValue(configConnStr);
                configTargeter->setFindHostReturnValue(configConnStr.getServers()[0]);

                return Status::OK();
            });

        _dbDirectClient = stdx::make_unique<DBDirectClient>(operationContext());
    }

    void tearDown() override {
        _dbDirectClient.reset();

        // Restore the defaults before calling tearDown
        storageGlobalParams.readOnly = false;
        serverGlobalParams.overrideShardIdentity = BSONObj();

        CatalogCacheLoader::clearForTests(getServiceContext());
        ShardingState::get(getServiceContext())->clearForTests();

        ShardingMongodTestFixture::tearDown();
    }

    std::unique_ptr<DistLockManager> makeDistLockManager(
        std::unique_ptr<DistLockCatalog> distLockCatalog) override {
        return stdx::make_unique<DistLockManagerMock>(nullptr);
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override {
        invariant(distLockManager);
        return stdx::make_unique<ShardingCatalogClientImpl>(std::move(distLockManager));
    }

    auto* shardingInitialization() {
        return ShardingInitializationMongoD::get(getServiceContext());
    }

    auto* shardingState() {
        return ShardingState::get(getServiceContext());
    }
};

/**
 * This class emulates the server being started as a standalone node for the scope for which it is
 * used
 */
class ScopedSetStandaloneMode {
public:
    ScopedSetStandaloneMode(ServiceContext* serviceContext) : _serviceContext(serviceContext) {
        serverGlobalParams.clusterRole = ClusterRole::None;
        _serviceContext->setOpObserver(stdx::make_unique<OpObserverRegistry>());
    }

    ~ScopedSetStandaloneMode() {
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        auto makeOpObserver = [&] {
            auto opObserver = stdx::make_unique<OpObserverRegistry>();
            opObserver->addObserver(stdx::make_unique<OpObserverShardingImpl>());
            opObserver->addObserver(stdx::make_unique<ConfigServerOpObserver>());
            opObserver->addObserver(stdx::make_unique<ShardServerOpObserver>());
            return opObserver;
        };

        _serviceContext->setOpObserver(makeOpObserver());
    }

private:
    ServiceContext* const _serviceContext;
};

TEST_F(ShardingInitializationMongoDTest, ValidShardIdentitySucceeds) {
    // Must hold a lock to call initializeFromShardIdentity.
    Lock::GlobalWrite lk(operationContext());

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(kShardName);
    shardIdentity.setClusterId(OID::gen());

    shardingInitialization()->initializeFromShardIdentity(operationContext(), shardIdentity);
    ASSERT_OK(shardingState()->canAcceptShardedCommands());
    ASSERT(shardingState()->enabled());
    ASSERT_EQ(kShardName, shardingState()->shardId());
    ASSERT_EQ("config/a:1,b:2", shardRegistry()->getConfigServerConnectionString().toString());
}

TEST_F(ShardingInitializationMongoDTest, InitWhilePreviouslyInErrorStateWillStayInErrorState) {
    // Must hold a lock to call initializeFromShardIdentity.
    Lock::GlobalWrite lk(operationContext());

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(kShardName);
    shardIdentity.setClusterId(OID::gen());

    shardingInitialization()->setGlobalInitMethodForTest([](
        OperationContext* opCtx, const ShardIdentity& shardIdentity, StringData distLockProcessId) {
        uasserted(ErrorCodes::ShutdownInProgress, "Not an actual shutdown");
    });

    shardingInitialization()->initializeFromShardIdentity(operationContext(), shardIdentity);

    // ShardingState is now in error state, attempting to call it again will still result in error.
    shardingInitialization()->setGlobalInitMethodForTest([](
        OperationContext* opCtx, const ShardIdentity& shardIdentity, StringData distLockProcessId) {
        FAIL("Should not be invoked!");
    });

    ASSERT_THROWS_CODE(
        shardingInitialization()->initializeFromShardIdentity(operationContext(), shardIdentity),
        AssertionException,
        ErrorCodes::ManualInterventionRequired);
    ASSERT_NOT_OK(shardingState()->canAcceptShardedCommands());
    ASSERT(!shardingState()->enabled());
}

TEST_F(ShardingInitializationMongoDTest, InitializeAgainWithMatchingShardIdentitySucceeds) {
    // Must hold a lock to call initializeFromShardIdentity.
    Lock::GlobalWrite lk(operationContext());

    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(kShardName);
    shardIdentity.setClusterId(clusterID);

    shardingInitialization()->initializeFromShardIdentity(operationContext(), shardIdentity);

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity2.setShardName(kShardName);
    shardIdentity2.setClusterId(clusterID);

    shardingInitialization()->setGlobalInitMethodForTest([](
        OperationContext* opCtx, const ShardIdentity& shardIdentity, StringData distLockProcessId) {
        FAIL("Should not be invoked!");
    });

    shardingInitialization()->initializeFromShardIdentity(operationContext(), shardIdentity2);

    ASSERT_OK(shardingState()->canAcceptShardedCommands());
    ASSERT_TRUE(shardingState()->enabled());

    ASSERT_EQ(kShardName, shardingState()->shardId());
    ASSERT_EQ("config/a:1,b:2", shardRegistry()->getConfigServerConnectionString().toString());
}

TEST_F(ShardingInitializationMongoDTest, InitializeAgainWithMatchingReplSetNameSucceeds) {
    // Must hold a lock to call initializeFromShardIdentity.
    Lock::GlobalWrite lk(operationContext());

    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(kShardName);
    shardIdentity.setClusterId(clusterID);

    shardingInitialization()->initializeFromShardIdentity(operationContext(), shardIdentity);

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::SET, "b:2,c:3", "config"));
    shardIdentity2.setShardName(kShardName);
    shardIdentity2.setClusterId(clusterID);

    shardingInitialization()->setGlobalInitMethodForTest([](
        OperationContext* opCtx, const ShardIdentity& shardIdentity, StringData distLockProcessId) {
        FAIL("Should not be invoked!");
    });

    shardingInitialization()->initializeFromShardIdentity(operationContext(), shardIdentity2);

    ASSERT_OK(shardingState()->canAcceptShardedCommands());
    ASSERT_TRUE(shardingState()->enabled());

    ASSERT_EQ(kShardName, shardingState()->shardId());
    ASSERT_EQ("config/a:1,b:2", shardRegistry()->getConfigServerConnectionString().toString());
}

// The tests below check for different combinations of the compatible startup parameters for
// --shardsvr, --overrideShardIdentity, and queryableBackup (readOnly) mode

/**
 * readOnly and --shardsvr
 */
TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededReadOnlyAndShardServerAndNoOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;

    ASSERT_THROWS_CODE(
        shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()),
        AssertionException,
        ErrorCodes::InvalidOptions);
}

TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededReadOnlyAndShardServerAndInvalidOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;
    serverGlobalParams.overrideShardIdentity =
        BSON("_id"
             << "shardIdentity"
             << ShardIdentity::kShardNameFieldName
             << kShardName
             << ShardIdentity::kClusterIdFieldName
             << OID::gen()
             << ShardIdentity::kConfigsvrConnectionStringFieldName
             << "invalid");

    ASSERT_THROWS_CODE(
        shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()),
        AssertionException,
        ErrorCodes::UnsupportedFormat);
}

TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededReadOnlyAndShardServerAndValidOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    serverGlobalParams.overrideShardIdentity = [] {
        ShardIdentityType shardIdentity;
        shardIdentity.setConfigsvrConnectionString(
            ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
        shardIdentity.setShardName(kShardName);
        shardIdentity.setClusterId(OID::gen());
        ASSERT_OK(shardIdentity.validate());
        return shardIdentity.toShardIdentityDocument();
    }();

    ASSERT(shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()));
}

/**
 * readOnly and not --shardsvr
 */
TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededReadOnlyAndNotShardServerAndNoOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;
    serverGlobalParams.clusterRole = ClusterRole::None;

    ASSERT(!shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()));
}

TEST_F(
    ShardingInitializationMongoDTest,
    InitializeShardingAwarenessIfNeededReadOnlyAndNotShardServerAndInvalidOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;
    serverGlobalParams.clusterRole = ClusterRole::None;
    serverGlobalParams.overrideShardIdentity = BSON("_id"
                                                    << "shardIdentity"
                                                    << "configsvrConnectionString"
                                                    << "invalid");

    ASSERT_THROWS_CODE(
        shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()),
        AssertionException,
        ErrorCodes::InvalidOptions);
}

TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededReadOnlyAndNotShardServerAndValidOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;
    serverGlobalParams.clusterRole = ClusterRole::None;
    serverGlobalParams.overrideShardIdentity = [] {
        ShardIdentityType shardIdentity;
        shardIdentity.setConfigsvrConnectionString(
            ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
        shardIdentity.setShardName(kShardName);
        shardIdentity.setClusterId(OID::gen());
        ASSERT_OK(shardIdentity.validate());
        return shardIdentity.toShardIdentityDocument();
    }();

    ASSERT_THROWS_CODE(
        shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()),
        AssertionException,
        ErrorCodes::InvalidOptions);
}

/**
 * not readOnly and --overrideShardIdentity
 */
TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndInvalidOverrideShardIdentity) {
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    serverGlobalParams.overrideShardIdentity = BSON("_id"
                                                    << "shardIdentity"
                                                    << "configsvrConnectionString"
                                                    << "invalid");

    ASSERT_THROWS_CODE(
        shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()),
        AssertionException,
        ErrorCodes::InvalidOptions);

    // Should error regardless of cluster role
    serverGlobalParams.clusterRole = ClusterRole::None;
    ASSERT_THROWS_CODE(
        shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()),
        AssertionException,
        ErrorCodes::InvalidOptions);
}

TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndValidOverrideShardIdentity) {
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    serverGlobalParams.overrideShardIdentity = [] {
        ShardIdentityType shardIdentity;
        shardIdentity.setConfigsvrConnectionString(
            ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
        shardIdentity.setShardName(kShardName);
        shardIdentity.setClusterId(OID::gen());
        ASSERT_OK(shardIdentity.validate());
        return shardIdentity.toShardIdentityDocument();
    }();

    ASSERT_THROWS_CODE(
        shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()),
        AssertionException,
        ErrorCodes::InvalidOptions);

    // Should error regardless of cluster role
    serverGlobalParams.clusterRole = ClusterRole::None;
    ASSERT_THROWS_CODE(
        shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()),
        AssertionException,
        ErrorCodes::InvalidOptions);
}

/**
 * not readOnly and --shardsvr
 */
TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndShardServerAndNoShardIdentity) {
    ASSERT(!shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()));
}

TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndShardServerAndInvalidShardIdentity) {
    // Insert the shardIdentity doc to disk while pretending that we are in "standalone" mode,
    // otherwise OpObserver for inserts will prevent the insert from occurring because the
    // shardIdentity doc is invalid
    {
        ScopedSetStandaloneMode standalone(getServiceContext());

        BSONObj invalidShardIdentity = BSON("_id"
                                            << "shardIdentity"
                                            << ShardIdentity::kShardNameFieldName
                                            << kShardName
                                            << ShardIdentity::kClusterIdFieldName
                                            << OID::gen()
                                            << ShardIdentity::kConfigsvrConnectionStringFieldName
                                            << "invalid");

        _dbDirectClient->insert(NamespaceString::kServerConfigurationNamespace.toString(),
                                invalidShardIdentity);
    }

    ASSERT_THROWS_CODE(
        shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()),
        AssertionException,
        ErrorCodes::UnsupportedFormat);
}

TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndShardServerAndValidShardIdentity) {
    // Insert the shardIdentity doc to disk while pretending that we are in "standalone" mode,
    // otherwise OpObserver for inserts will prevent the insert from occurring because the
    // shardIdentity doc is invalid
    {
        ScopedSetStandaloneMode standalone(getServiceContext());

        BSONObj validShardIdentity = [&] {
            ShardIdentityType shardIdentity;
            shardIdentity.setConfigsvrConnectionString(
                ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
            shardIdentity.setShardName(kShardName);
            shardIdentity.setClusterId(OID::gen());
            ASSERT_OK(shardIdentity.validate());
            return shardIdentity.toShardIdentityDocument();
        }();

        _dbDirectClient->insert(NamespaceString::kServerConfigurationNamespace.toString(),
                                validShardIdentity);
    }

    ASSERT(shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()));
}

/**
 * not readOnly and not --shardsvr
 */
TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndNotShardServerAndNoShardIdentity) {
    ScopedSetStandaloneMode standalone(getServiceContext());

    ASSERT(!shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()));
}

TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndNotShardServerAndInvalidShardIdentity) {
    ScopedSetStandaloneMode standalone(getServiceContext());

    _dbDirectClient->insert(NamespaceString::kServerConfigurationNamespace.toString(),
                            BSON("_id"
                                 << "shardIdentity"
                                 << "configsvrConnectionString"
                                 << "invalid"));

    // The shardIdentity doc on disk, even if invalid, is ignored if the ClusterRole is None. This
    // is to allow fixing the shardIdentity doc by starting without --shardsvr.
    ASSERT(!shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()));
}

TEST_F(ShardingInitializationMongoDTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndNotShardServerAndValidShardIdentity) {
    ScopedSetStandaloneMode standalone(getServiceContext());

    BSONObj validShardIdentity = [&] {
        ShardIdentityType shardIdentity;
        shardIdentity.setConfigsvrConnectionString(
            ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
        shardIdentity.setShardName(kShardName);
        shardIdentity.setClusterId(OID::gen());
        ASSERT_OK(shardIdentity.validate());
        return shardIdentity.toShardIdentityDocument();
    }();

    _dbDirectClient->insert(NamespaceString::kServerConfigurationNamespace.toString(),
                            validShardIdentity);

    // The shardIdentity doc on disk, even if invalid, is ignored if the ClusterRole is None. This
    // is to allow fixing the shardIdentity doc by starting without --shardsvr.
    ASSERT(!shardingInitialization()->initializeShardingAwarenessIfNeeded(operationContext()));
}

}  // namespace
}  // namespace mongo
