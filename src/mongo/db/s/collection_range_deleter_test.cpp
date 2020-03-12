
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/query.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

using Deletion = CollectionRangeDeleter::Deletion;

const NamespaceString kNss = NamespaceString("foo", "bar");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);
const NamespaceString kAdminSysVer = NamespaceString("admin", "system.version");

class CollectionRangeDeleterTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();

        // Make every test run with a separate epoch
        _epoch = OID::gen();

        DBDirectClient client(operationContext());
        client.createCollection(kNss.ns());

        const KeyPattern keyPattern(kShardKeyPattern);
        auto rt = RoutingTableHistory::makeNew(
            kNss,
            UUID::gen(),
            keyPattern,
            nullptr,
            false,
            epoch(),
            {ChunkType(kNss,
                       ChunkRange{keyPattern.globalMin(), keyPattern.globalMax()},
                       ChunkVersion(1, 0, epoch()),
                       ShardId("otherShard"))});
        std::shared_ptr<ChunkManager> cm = std::make_shared<ChunkManager>(rt, Timestamp(100, 0));

        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);
        auto* const css = CollectionShardingRuntime::get(operationContext(), kNss);
        css->setFilteringMetadata(operationContext(), CollectionMetadata(cm, ShardId("thisShard")));
    }

    void tearDown() override {
        {
            AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);
            auto* const css = CollectionShardingRuntime::get(operationContext(), kNss);
            css->clearFilteringMetadata();
        }

        ShardServerTestFixture::tearDown();
    }

    boost::optional<Date_t> next(CollectionRangeDeleter& rangeDeleter, int maxToDelete) {
        return CollectionRangeDeleter::cleanUpNextRange(
            operationContext(), kNss, epoch(), maxToDelete, &rangeDeleter);
    }

    std::shared_ptr<RemoteCommandTargeterMock> configTargeter() const {
        return RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
    }

    OID const& epoch() const {
        return _epoch;
    }

    std::unique_ptr<BalancerConfiguration> makeBalancerConfiguration() override {
        return stdx::make_unique<BalancerConfiguration>();
    }

private:
    OID _epoch;
};

// Tests the case that there is nothing in the database.
TEST_F(CollectionRangeDeleterTest, EmptyDatabase) {
    CollectionRangeDeleter rangeDeleter;
    ASSERT_FALSE(next(rangeDeleter, 1));
}

// Tests the case that there is data, but it is not in a range to clean.
TEST_F(CollectionRangeDeleterTest, NoDataInGivenRangeToClean) {
    CollectionRangeDeleter rangeDeleter;
    const BSONObj insertedDoc = BSON(kShardKey << 25);
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), insertedDoc);
    ASSERT_BSONOBJ_EQ(insertedDoc, dbclient.findOne(kNss.toString(), QUERY(kShardKey << 25)));
    std::list<Deletion> ranges;
    ranges.emplace_back(
        Deletion{ChunkRange{BSON(kShardKey << 0), BSON(kShardKey << 10)}, Date_t{}});
    auto when = rangeDeleter.add(std::move(ranges));
    ASSERT(when && *when == Date_t{});
    ASSERT_EQ(1u, rangeDeleter.size());
    ASSERT_TRUE(next(rangeDeleter, 1));

    ASSERT_EQ(0u, rangeDeleter.size());
    ASSERT_BSONOBJ_EQ(insertedDoc, dbclient.findOne(kNss.toString(), QUERY(kShardKey << 25)));

    ASSERT_FALSE(next(rangeDeleter, 1));
}

// Tests the case that there is a single document within a range to clean.
TEST_F(CollectionRangeDeleterTest, OneDocumentInOneRangeToClean) {
    CollectionRangeDeleter rangeDeleter;
    const BSONObj insertedDoc = BSON(kShardKey << 5);
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 5));
    ASSERT_BSONOBJ_EQ(insertedDoc, dbclient.findOne(kNss.toString(), QUERY(kShardKey << 5)));

    std::list<Deletion> ranges;
    auto deletion = Deletion{ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 10)), Date_t{}};
    ranges.emplace_back(std::move(deletion));
    auto when = rangeDeleter.add(std::move(ranges));
    ASSERT(when && *when == Date_t{});
    ASSERT_TRUE(ranges.empty());  // spliced elements out of it

    auto optNotifn = rangeDeleter.overlaps(ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 10)));
    ASSERT(optNotifn);
    auto notifn = *optNotifn;
    ASSERT(!notifn.ready());
    // actually delete one
    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT(!notifn.ready());

    ASSERT_EQ(rangeDeleter.size(), 1u);
    // range empty, pop range, notify
    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT_TRUE(rangeDeleter.isEmpty());
    ASSERT(notifn.ready() && notifn.waitStatus(operationContext()).isOK());

    ASSERT_TRUE(dbclient.findOne(kNss.toString(), QUERY(kShardKey << 5)).isEmpty());
    ASSERT_FALSE(next(rangeDeleter, 1));
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kShardKey << "startRangeDeletion")));
}

// Tests the case that there are multiple documents within a range to clean.
TEST_F(CollectionRangeDeleterTest, MultipleDocumentsInOneRangeToClean) {
    CollectionRangeDeleter rangeDeleter;
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 1));
    dbclient.insert(kNss.toString(), BSON(kShardKey << 2));
    dbclient.insert(kNss.toString(), BSON(kShardKey << 3));
    ASSERT_EQUALS(3ULL, dbclient.count(kNss.toString(), BSON(kShardKey << LT << 5)));

    std::list<Deletion> ranges;
    auto deletion = Deletion{ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 10)), Date_t{}};
    ranges.emplace_back(std::move(deletion));
    auto when = rangeDeleter.add(std::move(ranges));
    ASSERT(when && *when == Date_t{});

    ASSERT_TRUE(next(rangeDeleter, 100));
    ASSERT_TRUE(next(rangeDeleter, 100));
    ASSERT_EQUALS(0ULL, dbclient.count(kNss.toString(), BSON(kShardKey << LT << 5)));
    ASSERT_FALSE(next(rangeDeleter, 100));
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kShardKey << "startRangeDeletion")));
}

// Tests the case that there are multiple documents within a range to clean, and the range deleter
// has a max deletion rate of one document per run.
TEST_F(CollectionRangeDeleterTest, MultipleCleanupNextRangeCalls) {
    CollectionRangeDeleter rangeDeleter;
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 1));
    dbclient.insert(kNss.toString(), BSON(kShardKey << 2));
    dbclient.insert(kNss.toString(), BSON(kShardKey << 3));
    ASSERT_EQUALS(3ULL, dbclient.count(kNss.toString(), BSON(kShardKey << LT << 5)));

    std::list<Deletion> ranges;
    auto deletion = Deletion{ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 10)), Date_t{}};
    ranges.emplace_back(std::move(deletion));
    auto when = rangeDeleter.add(std::move(ranges));
    ASSERT(when && *when == Date_t{});

    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT_EQUALS(2ULL, dbclient.count(kNss.toString(), BSON(kShardKey << LT << 5)));

    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT_EQUALS(1ULL, dbclient.count(kNss.toString(), BSON(kShardKey << LT << 5)));

    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT_TRUE(next(rangeDeleter, 1));
    ASSERT_EQUALS(0ULL, dbclient.count(kNss.toString(), BSON(kShardKey << LT << 5)));
    ASSERT_FALSE(next(rangeDeleter, 1));
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kShardKey << "startRangeDeletion")));
}

// Tests the case that there are two ranges to clean, each containing multiple documents.
TEST_F(CollectionRangeDeleterTest, MultipleDocumentsInMultipleRangesToClean) {
    CollectionRangeDeleter rangeDeleter;
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 1));
    dbclient.insert(kNss.toString(), BSON(kShardKey << 2));
    dbclient.insert(kNss.toString(), BSON(kShardKey << 3));
    dbclient.insert(kNss.toString(), BSON(kShardKey << 4));
    dbclient.insert(kNss.toString(), BSON(kShardKey << 5));
    dbclient.insert(kNss.toString(), BSON(kShardKey << 6));
    ASSERT_EQUALS(6ULL, dbclient.count(kNss.toString(), BSON(kShardKey << LT << 10)));

    std::list<Deletion> ranges;
    auto later = Date_t::now();
    ranges.emplace_back(Deletion{ChunkRange{BSON(kShardKey << 0), BSON(kShardKey << 3)}, later});
    auto when = rangeDeleter.add(std::move(ranges));
    ASSERT(when && *when == later);
    ASSERT_TRUE(ranges.empty());  // not guaranteed by std, but failure would indicate a problem.

    std::list<Deletion> ranges2;
    ranges2.emplace_back(Deletion{ChunkRange{BSON(kShardKey << 4), BSON(kShardKey << 7)}, later});
    when = rangeDeleter.add(std::move(ranges2));
    ASSERT(!when);

    std::list<Deletion> ranges3;
    ranges3.emplace_back(
        Deletion{ChunkRange{BSON(kShardKey << 3), BSON(kShardKey << 4)}, Date_t{}});
    when = rangeDeleter.add(std::move(ranges3));
    ASSERT(when);

    auto optNotifn1 = rangeDeleter.overlaps(ChunkRange{BSON(kShardKey << 0), BSON(kShardKey << 3)});
    ASSERT_TRUE(optNotifn1);
    auto& notifn1 = *optNotifn1;
    ASSERT_FALSE(notifn1.ready());

    auto optNotifn2 = rangeDeleter.overlaps(ChunkRange{BSON(kShardKey << 4), BSON(kShardKey << 7)});
    ASSERT_TRUE(optNotifn2);
    auto& notifn2 = *optNotifn2;
    ASSERT_FALSE(notifn2.ready());

    auto optNotifn3 = rangeDeleter.overlaps(ChunkRange{BSON(kShardKey << 3), BSON(kShardKey << 4)});
    ASSERT_TRUE(optNotifn3);
    auto& notifn3 = *optNotifn3;
    ASSERT_FALSE(notifn3.ready());

    // test op== on notifications
    ASSERT_TRUE(notifn1 == *optNotifn1);
    ASSERT_FALSE(notifn1 == *optNotifn2);
    ASSERT_TRUE(notifn1 != *optNotifn2);
    ASSERT_FALSE(notifn1 != *optNotifn1);

    // no op log entry yet
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kShardKey << "startRangeDeletion")));

    ASSERT_EQUALS(6ULL, dbclient.count(kNss.ns(), BSON(kShardKey << LT << 7)));

    // catch range3, [3..4) only
    auto next1 = next(rangeDeleter, 100);
    ASSERT_TRUE(next1);

    // no op log entry for immediate deletions
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kShardKey << "startRangeDeletion")));

    // 3 gone
    ASSERT_EQUALS(5ULL, dbclient.count(kNss.ns(), BSON(kShardKey << LT << 7)));
    ASSERT_EQUALS(2ULL, dbclient.count(kNss.ns(), BSON(kShardKey << LT << 4)));

    ASSERT_FALSE(notifn1.ready());  // no trigger yet
    ASSERT_FALSE(notifn2.ready());  // no trigger yet
    ASSERT_FALSE(notifn3.ready());  // no trigger yet

    // this will find the [3..4) range empty, so pop the range and notify
    auto next2 = next(rangeDeleter, 100);
    ASSERT_TRUE(next2);

    // still no op log entry, because not delayed
    ASSERT_EQUALS(0ULL, dbclient.count(kAdminSysVer.ns(), BSON(kShardKey << "startRangeDeletion")));

    // deleted 1, 5 left
    ASSERT_EQUALS(2ULL, dbclient.count(kNss.ns(), BSON(kShardKey << LT << 4)));
    ASSERT_EQUALS(5ULL, dbclient.count(kNss.ns(), BSON(kShardKey << LT << 10)));

    ASSERT_FALSE(notifn1.ready());  // no trigger yet
    ASSERT_FALSE(notifn2.ready());  // no trigger yet
    ASSERT_TRUE(notifn3.ready());   // triggered.
    ASSERT_OK(notifn3.waitStatus(operationContext()));

    // This will find the regular queue empty, but the [0..3) range in the delayed queue.
    // However, the time to delete them is now, so the range is moved to the regular queue.
    auto next3 = next(rangeDeleter, 100);
    ASSERT_TRUE(next3);

    ASSERT_FALSE(notifn1.ready());  // no trigger yet
    ASSERT_FALSE(notifn2.ready());  // no trigger yet

    // deleted 3, 3 left
    ASSERT_EQUALS(3ULL, dbclient.count(kNss.ns(), BSON(kShardKey << LT << 10)));

    ASSERT_EQUALS(1ULL, dbclient.count(kAdminSysVer.ns(), BSON(kShardKey << "startRangeDeletion")));
    // clang-format off
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << "startRangeDeletion" << "ns" << kNss.ns()
          << "epoch" << epoch() << "min" << BSON("_id" << 0) << "max" << BSON("_id" << 3)),
        dbclient.findOne(kAdminSysVer.ns(), QUERY("_id" << "startRangeDeletion")));
    // clang-format on

    // this will find the [0..3) range empty, so pop the range and notify
    auto next4 = next(rangeDeleter, 100);
    ASSERT_TRUE(next4);

    ASSERT_TRUE(notifn1.ready());
    ASSERT_OK(notifn1.waitStatus(operationContext()));
    ASSERT_FALSE(notifn2.ready());

    // op log entry unchanged
    // clang-format off
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << "startRangeDeletion" << "ns" << kNss.ns()
          << "epoch" << epoch() << "min" << BSON("_id" << 0) << "max" << BSON("_id" << 3)),
        dbclient.findOne(kAdminSysVer.ns(), QUERY("_id" << "startRangeDeletion")));
    // clang-format on

    // still 3 left
    ASSERT_EQUALS(3ULL, dbclient.count(kNss.ns(), BSON(kShardKey << LT << 10)));

    // delete the remaining documents
    auto next5 = next(rangeDeleter, 100);
    ASSERT_TRUE(next5);

    ASSERT_FALSE(notifn2.ready());

    // Another delayed range, so logged
    // clang-format off
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << "startRangeDeletion" << "ns" << kNss.ns()
          << "epoch" << epoch() << "min" << BSON("_id" << 4) << "max" << BSON("_id" << 7)),
        dbclient.findOne(kAdminSysVer.ns(), QUERY("_id" << "startRangeDeletion")));
    // clang-format on

    // all docs gone
    ASSERT_EQUALS(0ULL, dbclient.count(kNss.ns(), BSON(kShardKey << LT << 10)));

    // discover there are no more, pop range 2
    auto next6 = next(rangeDeleter, 100);
    ASSERT_TRUE(next6);

    ASSERT_TRUE(notifn2.ready());
    ASSERT_OK(notifn2.waitStatus(operationContext()));

    // discover there are no more ranges
    ASSERT_FALSE(next(rangeDeleter, 1));
}

}  // namespace
}  // namespace mongo
