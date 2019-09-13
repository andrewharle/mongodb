
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/s/active_shard_collection_registry.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using unittest::assertGet;

class ShardCollectionRegistrationTest : public ServiceContextMongoDTest {
protected:
    ActiveShardCollectionRegistry _registry;
};

ShardsvrShardCollection createShardsvrShardCollectionRequest(
    const NamespaceString& nss,
    BSONObj key,
    bool unique,
    int numInitialChunks,
    boost::optional<std::vector<mongo::BSONObj>> initialSplitPoints,
    boost::optional<mongo::BSONObj> collation,
    bool UUIDfromPrimaryShard) {
    ShardsvrShardCollection shardsvrShardCollectionRequest;
    shardsvrShardCollectionRequest.set_shardsvrShardCollection(nss);
    shardsvrShardCollectionRequest.setKey(key);
    shardsvrShardCollectionRequest.setUnique(unique);
    shardsvrShardCollectionRequest.setNumInitialChunks(numInitialChunks);
    shardsvrShardCollectionRequest.setInitialSplitPoints(initialSplitPoints);
    shardsvrShardCollectionRequest.setCollation(collation);
    shardsvrShardCollectionRequest.setGetUUIDfromPrimaryShard(UUIDfromPrimaryShard);

    return shardsvrShardCollectionRequest;
}

TEST_F(ShardCollectionRegistrationTest, ScopedShardCollectionConstructorAndAssignment) {
    auto shardsvrShardCollectionRequest =
        createShardsvrShardCollectionRequest(NamespaceString("TestDB", "TestColl"),
                                             BSON("x"
                                                  << "hashed"),
                                             false,
                                             1,
                                             boost::none,
                                             boost::none,
                                             false);
    auto originalScopedShardCollection =
        assertGet(_registry.registerShardCollection(shardsvrShardCollectionRequest));
    ASSERT(originalScopedShardCollection.mustExecute());

    ScopedShardCollection movedScopedShardCollection(std::move(originalScopedShardCollection));
    ASSERT(movedScopedShardCollection.mustExecute());

    originalScopedShardCollection = std::move(movedScopedShardCollection);
    ASSERT(originalScopedShardCollection.mustExecute());

    // Need to signal the registered shard collection so the destructor doesn't invariant
    originalScopedShardCollection.signalComplete(Status::OK());
}

TEST_F(ShardCollectionRegistrationTest,
       SecondShardCollectionWithDifferentOptionsReturnsConflictingOperationInProgress) {
    auto firstShardsvrShardCollectionRequest =
        createShardsvrShardCollectionRequest(NamespaceString("TestDB", "TestColl"),
                                             BSON("x"
                                                  << "hashed"),
                                             false,
                                             1,
                                             boost::none,
                                             boost::none,
                                             false);
    auto originalScopedShardCollection =
        assertGet(_registry.registerShardCollection(firstShardsvrShardCollectionRequest));

    auto secondShardsvrShardCollectionRequest =
        createShardsvrShardCollectionRequest(NamespaceString("TestDB", "TestColl"),
                                             BSON("x" << 0),
                                             false,
                                             1,
                                             boost::none,
                                             boost::none,
                                             false);
    auto secondScopedShardCollection =
        _registry.registerShardCollection(secondShardsvrShardCollectionRequest);
    ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, secondScopedShardCollection.getStatus());

    originalScopedShardCollection.signalComplete(Status::OK());
}

TEST_F(ShardCollectionRegistrationTest, SecondShardCollectionWithSameOptionsJoinsFirst) {
    auto firstShardsvrShardCollectionRequest =
        createShardsvrShardCollectionRequest(NamespaceString("TestDB", "TestColl"),
                                             BSON("x"
                                                  << "hashed"),
                                             false,
                                             1,
                                             boost::none,
                                             boost::none,
                                             false);
    auto originalScopedShardCollection =
        assertGet(_registry.registerShardCollection(firstShardsvrShardCollectionRequest));
    ASSERT(originalScopedShardCollection.mustExecute());

    auto secondShardsvrShardCollectionRequest =
        createShardsvrShardCollectionRequest(NamespaceString("TestDB", "TestColl"),
                                             BSON("x"
                                                  << "hashed"),
                                             false,
                                             1,
                                             boost::none,
                                             boost::none,
                                             false);
    auto secondScopedShardCollection =
        assertGet(_registry.registerShardCollection(secondShardsvrShardCollectionRequest));
    ASSERT(!secondScopedShardCollection.mustExecute());

    originalScopedShardCollection.signalComplete({ErrorCodes::InternalError, "Test error"});
    auto opCtx = makeOperationContext();
    ASSERT_EQ(Status(ErrorCodes::InternalError, "Test error"),
              secondScopedShardCollection.waitForCompletion(opCtx.get()));
}

TEST_F(ShardCollectionRegistrationTest, TwoShardCollectionsOnDifferentCollectionsAllowed) {
    auto firstShardsvrShardCollectionRequest =
        createShardsvrShardCollectionRequest(NamespaceString("TestDB", "TestColl"),
                                             BSON("x"
                                                  << "hashed"),
                                             false,
                                             1,
                                             boost::none,
                                             boost::none,
                                             false);
    auto originalScopedShardCollection =
        assertGet(_registry.registerShardCollection(firstShardsvrShardCollectionRequest));
    ASSERT(originalScopedShardCollection.mustExecute());

    auto secondShardsvrShardCollectionRequest =
        createShardsvrShardCollectionRequest(NamespaceString("TestDB2", "TestColl2"),
                                             BSON("x"
                                                  << "hashed"),
                                             false,
                                             1,
                                             boost::none,
                                             boost::none,
                                             false);
    auto secondScopedShardCollection =
        assertGet(_registry.registerShardCollection(secondShardsvrShardCollectionRequest));
    ASSERT(secondScopedShardCollection.mustExecute());

    originalScopedShardCollection.signalComplete(Status::OK());
    secondScopedShardCollection.signalComplete(Status::OK());
}

}  // namespace
}  // namespace mongo
