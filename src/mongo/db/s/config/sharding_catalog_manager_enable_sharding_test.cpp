
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

#include <pcrecpp.h>

#include "mongo/bson/json.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/future.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using std::vector;

class EnableShardingTest : public ConfigServerTestFixture {};

TEST_F(EnableShardingTest, noDBExists) {
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");

    setupShards(vector<ShardType>{shard});

    auto shardTargeter = RemoteCommandTargeterMock::get(
        uassertStatusOK(shardRegistry()->getShard(operationContext(), ShardId("shard0")))
            ->getTargeter());
    shardTargeter->setFindHostReturnValue(HostAndPort("shard0:12"));

    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ShardingCatalogManager::get(opCtx.get())->enableSharding(opCtx.get(), "db1");
    });

    // list databases for checking shard size.
    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("shard0:12"), request.target);
        ASSERT_EQ("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(BSON("listDatabases" << 1 << "maxTimeMS" << 600000), request.cmdObj);

        ASSERT_BSONOBJ_EQ(
            ReadPreferenceSetting(ReadPreference::PrimaryPreferred).toContainingBSON(),
            rpc::TrackingMetadata::removeTrackingData(request.metadata));

        return fromjson(R"({
                databases: [],
                totalSize: 1,
                ok: 1
            })");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(EnableShardingTest, dbExistsWithDifferentCase) {
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");
    setupShards(vector<ShardType>{shard});
    setupDatabase("Db3", shard.getName(), false);
    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(operationContext())->enableSharding(operationContext(), "db3"),
        AssertionException,
        ErrorCodes::DatabaseDifferCase);
}

TEST_F(EnableShardingTest, dbExists) {
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");
    setupShards(vector<ShardType>{shard});
    setupDatabase("db4", shard.getName(), false);
    ShardingCatalogManager::get(operationContext())->enableSharding(operationContext(), "db4");
}

TEST_F(EnableShardingTest, succeedsWhenTheDatabaseIsAlreadySharded) {
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");
    setupShards(vector<ShardType>{shard});
    setupDatabase("db5", shard.getName(), true);
    ShardingCatalogManager::get(operationContext())->enableSharding(operationContext(), "db5");
}

TEST_F(EnableShardingTest, dbExistsInvalidFormat) {
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");

    setupShards(vector<ShardType>{shard});

    // Set up database with bad type for primary field.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    DatabaseType::ConfigNS,
                                                    BSON("_id"
                                                         << "db6"
                                                         << "primary"
                                                         << 12
                                                         << "partitioned"
                                                         << false),
                                                    ShardingCatalogClient::kMajorityWriteConcern));

    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(operationContext())->enableSharding(operationContext(), "db6"),
        AssertionException,
        ErrorCodes::TypeMismatch);
}

TEST_F(EnableShardingTest, noDBExistsNoShards) {
    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(operationContext())->enableSharding(operationContext(), "db7"),
        AssertionException,
        ErrorCodes::ShardNotFound);
}

}  // namespace
}  // namespace mongo
