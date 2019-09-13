
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <set>

#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const NamespaceString kNss("TestDB", "TestColl");

class ChunkManagerQueryTest : public CatalogCacheTestFixture {
protected:
    void runGetShardIdsForRangeTest(const BSONObj& shardKey,
                                    bool unique,
                                    const std::vector<BSONObj>& splitPoints,
                                    const BSONObj& min,
                                    const BSONObj& max,
                                    const std::set<ShardId>& expectedShardIds) {
        const ShardKeyPattern shardKeyPattern(shardKey);
        auto chunkManager = makeChunkManager(kNss, shardKeyPattern, nullptr, false, splitPoints);

        std::set<ShardId> shardIds;
        chunkManager->getShardIdsForRange(min, max, &shardIds);

        _assertShardIdsMatch(expectedShardIds, shardIds);
    }

    void runQueryTest(const BSONObj& shardKey,
                      std::unique_ptr<CollatorInterface> defaultCollator,
                      bool unique,
                      const std::vector<BSONObj>& splitPoints,
                      const BSONObj& query,
                      const BSONObj& queryCollation,
                      const std::set<ShardId>& expectedShardIds) {
        const ShardKeyPattern shardKeyPattern(shardKey);
        auto chunkManager =
            makeChunkManager(kNss, shardKeyPattern, std::move(defaultCollator), false, splitPoints);

        std::set<ShardId> shardIds;
        chunkManager->getShardIdsForQuery(operationContext(), query, queryCollation, &shardIds);

        _assertShardIdsMatch(expectedShardIds, shardIds);
    }

private:
    static void _assertShardIdsMatch(const std::set<ShardId>& expectedShardIds,
                                     const std::set<ShardId>& actualShardIds) {
        BSONArrayBuilder expectedBuilder;
        for (const auto& shardId : expectedShardIds) {
            expectedBuilder << shardId;
        }

        BSONArrayBuilder actualBuilder;
        for (const auto& shardId : actualShardIds) {
            actualBuilder << shardId;
        }

        ASSERT_BSONOBJ_EQ(expectedBuilder.arr(), actualBuilder.arr());
    }
};

TEST_F(ChunkManagerQueryTest, GetShardIdsForRangeMinAndMaxAreInclusive) {
    runGetShardIdsForRangeTest(BSON("a" << 1),
                               false,
                               {BSON("a" << -100), BSON("a" << 0), BSON("a" << 100)},
                               BSON("a" << -100),
                               BSON("a" << 0),
                               {ShardId("1"), ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, GetShardIdsForRangeMinAndMaxAreTheSameAtFirstChunkMaxBoundary) {
    runGetShardIdsForRangeTest(BSON("a" << 1),
                               false,
                               {BSON("a" << -100), BSON("a" << 0), BSON("a" << 100)},
                               BSON("a" << -100),
                               BSON("a" << -100),
                               {ShardId("1")});
}

TEST_F(ChunkManagerQueryTest, GetShardIdsForRangeMinAndMaxAreTheSameAtLastChunkMinBoundary) {
    runGetShardIdsForRangeTest(BSON("a" << 1),
                               false,
                               {BSON("a" << -100), BSON("a" << 0), BSON("a" << 100)},
                               BSON("a" << 100),
                               BSON("a" << 100),
                               {ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, EmptyQuerySingleShard) {
    runQueryTest(BSON("a" << 1), nullptr, false, {}, BSONObj(), BSONObj(), {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, EmptyQueryMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSONObj(),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, UniversalRangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("b" << 1),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, EqualityRangeSingleShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {},
                 BSON("a"
                      << "x"),
                 BSONObj(),
                 {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, EqualityRangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a"
                      << "y"),
                 BSONObj(),
                 {ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, SetRangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 fromjson("{a:{$in:['u','y']}}"),
                 BSONObj(),
                 {ShardId("0"), ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, GTRangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << GT << "x"),
                 BSONObj(),
                 {ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, GTERangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << GTE << "x"),
                 BSONObj(),
                 {ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, LTRangeMultiShard) {
    // NOTE (SERVER-4791): It isn't actually necessary to return shard 2 because its lowest key is
    // "y", which is excluded from the query
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << LT << "y"),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, LTERangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << LTE << "y"),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, OrEqualities) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 fromjson("{$or:[{a:'u'},{a:'y'}]}"),
                 BSONObj(),
                 {ShardId("0"), ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, OrEqualityInequality) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 fromjson("{$or:[{a:'u'},{a:{$gte:'y'}}]}"),
                 BSONObj(),
                 {ShardId("0"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, OrEqualityInequalityUnhelpful) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 fromjson("{$or:[{a:'u'},{a:{$gte:'zz'}},{}]}"),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, UnsatisfiableRangeSingleShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {},
                 BSON("a" << GT << "x" << LT << "x"),
                 BSONObj(),
                 {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, UnsatisfiableRangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << GT << "x" << LT << "x"),
                 BSONObj(),
                 {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, EqualityThenUnsatisfiable) {
    runQueryTest(BSON("a" << 1 << "b" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << 1 << "b" << GT << 4 << LT << 4),
                 BSONObj(),
                 {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, InequalityThenUnsatisfiable) {
    runQueryTest(BSON("a" << 1 << "b" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << GT << 1 << "b" << GT << 4 << LT << 4),
                 BSONObj(),
                 {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, OrEqualityUnsatisfiableInequality) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 fromjson("{$or:[{a:'x'},{a:{$gt:'u',$lt:'u'}},{a:{$gte:'y'}}]}"),
                 BSONObj(),
                 {ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, InMultiShard) {
    runQueryTest(BSON("a" << 1 << "b" << 1),
                 nullptr,
                 false,
                 {BSON("a" << 5 << "b" << 10), BSON("a" << 5 << "b" << 20)},
                 BSON("a" << BSON("$in" << BSON_ARRAY(0 << 5 << 10)) << "b"
                          << BSON("$in" << BSON_ARRAY(0 << 5 << 25))),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, CollationStringsMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a"
                      << "y"),
                 BSON("locale"
                      << "mock_reverse_string"),
                 {ShardId("0"), ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, DefaultCollationStringsMultiShard) {
    runQueryTest(
        BSON("a" << 1),
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString),
        false,
        {BSON("a"
              << "x"),
         BSON("a"
              << "y"),
         BSON("a"
              << "z")},
        BSON("a"
             << "y"),
        BSON("locale"
             << "mock_reverse_string"),
        {ShardId("0"), ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, SimpleCollationStringsMultiShard) {
    runQueryTest(
        BSON("a" << 1),
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString),
        false,
        {BSON("a"
              << "x"),
         BSON("a"
              << "y"),
         BSON("a"
              << "z")},
        BSON("a"
             << "y"),
        BSON("locale"
             << "simple"),
        {ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, CollationNumbersMultiShard) {
    runQueryTest(
        BSON("a" << 1),
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString),
        false,
        {BSON("a"
              << "x"),
         BSON("a"
              << "y"),
         BSON("a"
              << "z")},
        BSON("a" << 5),
        BSON("locale"
             << "mock_reverse_string"),
        {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, DefaultCollationNumbersMultiShard) {
    runQueryTest(
        BSON("a" << 1),
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString),
        false,
        {BSON("a"
              << "x"),
         BSON("a"
              << "y"),
         BSON("a"
              << "z")},
        BSON("a" << 5),
        BSONObj(),
        {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, SimpleCollationNumbersMultiShard) {
    runQueryTest(
        BSON("a" << 1),
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString),
        false,
        {BSON("a"
              << "x"),
         BSON("a"
              << "y"),
         BSON("a"
              << "z")},
        BSON("a" << 5),
        BSON("locale"
             << "simple"),
        {ShardId("0")});
}

}  // namespace
}  // namespace mongo
