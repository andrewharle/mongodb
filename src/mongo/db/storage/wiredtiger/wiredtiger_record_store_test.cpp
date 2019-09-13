
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

#include <memory>
#include <sstream>
#include <string>
#include <time.h>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_stones.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using std::unique_ptr;
using std::string;
using std::stringstream;

TEST(WiredTigerRecordStoreTest, GenerateCreateStringEmptyDocument) {
    BSONObj spec = fromjson("{}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), "");  // "," would also be valid.
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringUnknownField) {
    BSONObj spec = fromjson("{unknownField: 1}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    const Status& status = result.getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, status);
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringNonStringConfig) {
    BSONObj spec = fromjson("{configString: 12345}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    const Status& status = result.getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringEmptyConfigString) {
    BSONObj spec = fromjson("{configString: ''}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), ",");  // "" would also be valid.
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringInvalidConfigStringOption) {
    BSONObj spec = fromjson("{configString: 'abc=def'}");
    ASSERT_EQ(WiredTigerRecordStore::parseOptionsField(spec), ErrorCodes::BadValue);
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringValidConfigStringOption) {
    BSONObj spec = fromjson("{configString: 'prefix_compression=true'}");
    ASSERT_EQ(WiredTigerRecordStore::parseOptionsField(spec),
              std::string("prefix_compression=true,"));
}

TEST(WiredTigerRecordStoreTest, Isolation1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    RecordId id1;
    RecordId id2;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();

            res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            id2 = res.getValue();

            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext t1(harnessHelper->newOperationContext());
        auto client2 = harnessHelper->serviceContext()->makeClient("c2");
        auto t2 = harnessHelper->newOperationContext(client2.get());

        unique_ptr<WriteUnitOfWork> w1(new WriteUnitOfWork(t1.get()));
        unique_ptr<WriteUnitOfWork> w2(new WriteUnitOfWork(t2.get()));

        rs->dataFor(t1.get(), id1);
        rs->dataFor(t2.get(), id1);

        ASSERT_OK(rs->updateRecord(t1.get(), id1, "b", 2, false, NULL));
        ASSERT_OK(rs->updateRecord(t1.get(), id2, "B", 2, false, NULL));

        try {
            // this should fail
            rs->updateRecord(t2.get(), id1, "c", 2, false, NULL).transitional_ignore();
            ASSERT(0);
        } catch (WriteConflictException&) {
            w2.reset(NULL);
            t2.reset(NULL);
        }

        w1->commit();  // this should succeed
    }
}

TEST(WiredTigerRecordStoreTest, Isolation2) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    RecordId id1;
    RecordId id2;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();

            res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            id2 = res.getValue();

            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext t1(harnessHelper->newOperationContext());
        auto client2 = harnessHelper->serviceContext()->makeClient("c2");
        auto t2 = harnessHelper->newOperationContext(client2.get());

        // ensure we start transactions
        rs->dataFor(t1.get(), id2);
        rs->dataFor(t2.get(), id2);

        {
            WriteUnitOfWork w(t1.get());
            ASSERT_OK(rs->updateRecord(t1.get(), id1, "b", 2, false, NULL));
            w.commit();
        }

        {
            WriteUnitOfWork w(t2.get());
            ASSERT_EQUALS(string("a"), rs->dataFor(t2.get(), id1).data());
            try {
                // this should fail as our version of id1 is too old
                rs->updateRecord(t2.get(), id1, "c", 2, false, NULL).transitional_ignore();
                ASSERT(0);
            } catch (WriteConflictException&) {
            }
        }
    }
}


StatusWith<RecordId> insertBSON(ServiceContext::UniqueOperationContext& opCtx,
                                unique_ptr<RecordStore>& rs,
                                const Timestamp& opTime) {
    BSONObj obj = BSON("ts" << opTime);
    WriteUnitOfWork wuow(opCtx.get());
    WiredTigerRecordStore* wrs = checked_cast<WiredTigerRecordStore*>(rs.get());
    invariant(wrs);
    Status status = wrs->oplogDiskLocRegister(opCtx.get(), opTime, false);
    if (!status.isOK())
        return StatusWith<RecordId>(status);
    StatusWith<RecordId> res =
        rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), opTime, false);
    if (res.isOK())
        wuow.commit();
    return res;
}

TEST(WiredTigerRecordStoreTest, CappedCursorRollover) {
    unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newCappedRecordStore("a.b", 10000, 5));

    {  // first insert 3 documents
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        for (int i = 0; i < 3; ++i) {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            uow.commit();
        }
    }

    // set up our cursor that should rollover

    auto client2 = harnessHelper->serviceContext()->makeClient("c2");
    auto cursorCtx = harnessHelper->newOperationContext(client2.get());
    auto cursor = rs->getCursor(cursorCtx.get());
    ASSERT(cursor->next());
    cursor->save();
    cursorCtx->recoveryUnit()->abandonSnapshot();

    {  // insert 100 documents which causes rollover
        auto client3 = harnessHelper->serviceContext()->makeClient("c3");
        auto opCtx = harnessHelper->newOperationContext(client3.get());
        for (int i = 0; i < 100; i++) {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            uow.commit();
        }
    }

    // cursor should now be dead
    ASSERT_FALSE(cursor->restore());
    ASSERT(!cursor->next());
}

RecordId _oplogOrderInsertOplog(OperationContext* opCtx,
                                const unique_ptr<RecordStore>& rs,
                                int inc) {
    Timestamp opTime = Timestamp(5, inc);
    Status status = rs->oplogDiskLocRegister(opCtx, opTime, false);
    ASSERT_OK(status);
    BSONObj obj = BSON("ts" << opTime);
    StatusWith<RecordId> res = rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), opTime, false);
    ASSERT_OK(res.getStatus());
    return res.getValue();
}

// Test that even when the oplog durability loop is paused, we can still advance the commit point as
// long as the commit for each insert comes before the next insert starts.
TEST(WiredTigerRecordStoreTest, OplogDurableVisibilityInOrder) {
    ON_BLOCK_EXIT([] { WTPausePrimaryOplogDurabilityLoop.setMode(FailPoint::off); });
    WTPausePrimaryOplogDurabilityLoop.setMode(FailPoint::alwaysOn);

    unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newCappedRecordStore("local.oplog.rs", 100000, -1));
    auto wtrs = checked_cast<WiredTigerRecordStore*>(rs.get());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        RecordId id = _oplogOrderInsertOplog(opCtx.get(), rs, 1);
        ASSERT(wtrs->isOpHidden_forTest(id));
        uow.commit();
        ASSERT(wtrs->isOpHidden_forTest(id));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        RecordId id = _oplogOrderInsertOplog(opCtx.get(), rs, 2);
        ASSERT(wtrs->isOpHidden_forTest(id));
        uow.commit();
        ASSERT(wtrs->isOpHidden_forTest(id));
    }
}

// Test that Oplog entries inserted while there are hidden entries do not become visible until the
// op and all earlier ops are durable.
TEST(WiredTigerRecordStoreTest, OplogDurableVisibilityOutOfOrder) {
    ON_BLOCK_EXIT([] { WTPausePrimaryOplogDurabilityLoop.setMode(FailPoint::off); });
    WTPausePrimaryOplogDurabilityLoop.setMode(FailPoint::alwaysOn);

    unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newCappedRecordStore("local.oplog.rs", 100000, -1));

    auto wtrs = checked_cast<WiredTigerRecordStore*>(rs.get());

    ServiceContext::UniqueOperationContext longLivedOp(harnessHelper->newOperationContext());
    WriteUnitOfWork uow(longLivedOp.get());
    RecordId id1 = _oplogOrderInsertOplog(longLivedOp.get(), rs, 1);
    ASSERT(wtrs->isOpHidden_forTest(id1));


    RecordId id2;
    {
        auto innerClient = harnessHelper->serviceContext()->makeClient("inner");
        ServiceContext::UniqueOperationContext opCtx(
            harnessHelper->newOperationContext(innerClient.get()));
        WriteUnitOfWork uow(opCtx.get());
        id2 = _oplogOrderInsertOplog(opCtx.get(), rs, 2);
        ASSERT(wtrs->isOpHidden_forTest(id2));
        uow.commit();
    }

    ASSERT(wtrs->isOpHidden_forTest(id1));
    ASSERT(wtrs->isOpHidden_forTest(id2));

    uow.commit();

    ASSERT(wtrs->isOpHidden_forTest(id1));
    ASSERT(wtrs->isOpHidden_forTest(id2));

    // Wait a bit and check again to make sure they don't become visible automatically.
    sleepsecs(1);
    ASSERT(wtrs->isOpHidden_forTest(id1));
    ASSERT(wtrs->isOpHidden_forTest(id2));

    WTPausePrimaryOplogDurabilityLoop.setMode(FailPoint::off);

    rs->waitForAllEarlierOplogWritesToBeVisible(longLivedOp.get());

    ASSERT(!wtrs->isOpHidden_forTest(id1));
    ASSERT(!wtrs->isOpHidden_forTest(id2));
}

TEST(WiredTigerRecordStoreTest, AppendCustomStatsMetadata) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore("a.b"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    BSONObjBuilder builder;
    rs->appendCustomStats(opCtx.get(), &builder, 1.0);
    BSONObj customStats = builder.obj();

    BSONElement wiredTigerElement = customStats.getField(kWiredTigerEngineName);
    ASSERT_TRUE(wiredTigerElement.isABSONObj());
    BSONObj wiredTiger = wiredTigerElement.Obj();

    BSONElement metadataElement = wiredTiger.getField("metadata");
    ASSERT_TRUE(metadataElement.isABSONObj());
    BSONObj metadata = metadataElement.Obj();

    BSONElement versionElement = metadata.getField("formatVersion");
    ASSERT_TRUE(versionElement.isNumber());

    BSONElement creationStringElement = wiredTiger.getField("creationString");
    ASSERT_EQUALS(creationStringElement.type(), String);
}

TEST(WiredTigerRecordStoreTest, CappedCursorYieldFirst) {
    unique_ptr<RecordStoreHarnessHelper> harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newCappedRecordStore("a.b", 10000, 50));

    RecordId id1;

    {  // first insert a document
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp(), false);
        ASSERT_OK(res.getStatus());
        id1 = res.getValue();
        uow.commit();
    }

    ServiceContext::UniqueOperationContext cursorCtx(harnessHelper->newOperationContext());
    auto cursor = rs->getCursor(cursorCtx.get());

    // See that things work if you yield before you first call next().
    cursor->save();
    cursorCtx->recoveryUnit()->abandonSnapshot();
    ASSERT_TRUE(cursor->restore());
    auto record = cursor->next();
    ASSERT(record);
    ASSERT_EQ(id1, record->id);
    ASSERT(!cursor->next());
}

BSONObj makeBSONObjWithSize(const Timestamp& opTime, int size, char fill = 'x') {
    BSONObj objTemplate = BSON("ts" << opTime << "str"
                                    << "");
    ASSERT_LTE(objTemplate.objsize(), size);
    std::string str(size - objTemplate.objsize(), fill);

    BSONObj obj = BSON("ts" << opTime << "str" << str);
    ASSERT_EQ(size, obj.objsize());

    return obj;
}

StatusWith<RecordId> insertBSONWithSize(OperationContext* opCtx,
                                        RecordStore* rs,
                                        const Timestamp& opTime,
                                        int size) {
    BSONObj obj = makeBSONObjWithSize(opTime, size);

    WriteUnitOfWork wuow(opCtx);
    WiredTigerRecordStore* wtrs = checked_cast<WiredTigerRecordStore*>(rs);
    invariant(wtrs);
    Status status = wtrs->oplogDiskLocRegister(opCtx, opTime, false);
    if (!status.isOK()) {
        return StatusWith<RecordId>(status);
    }
    StatusWith<RecordId> res = rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), opTime, false);
    if (res.isOK()) {
        wuow.commit();
    }
    return res;
}

// Insert records into an oplog and verify the number of stones that are created.
TEST(WiredTigerRecordStoreTest, OplogStones_CreateNewStone) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper->newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(0U, oplogStones->numStones());

        // Inserting a record smaller than 'minBytesPerStone' shouldn't create a new oplog stone.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 99), RecordId(1, 1));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(99, oplogStones->currentBytes());

        // Inserting another record such that their combined size exceeds 'minBytesPerStone' should
        // cause a new stone to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 51), RecordId(1, 2));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one exceed 'minBytesPerStone' shouldn't cause a new stone to be created because we've
        // started filling a new stone.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 50), RecordId(1, 3));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one is exactly equal to 'minBytesPerStone' should cause a new stone to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 4), 50), RecordId(1, 4));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());

        // Inserting a single record that exceeds 'minBytesPerStone' should cause a new stone to
        // be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 5), 101), RecordId(1, 5));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
}

// Insert records into an oplog and try to update them. The updates shouldn't succeed if the size of
// record is changed.
TEST(WiredTigerRecordStoreTest, OplogStones_UpdateRecord) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper->newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    // Insert two records such that one makes up a full stone and the other is a part of the stone
    // currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 100), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 50), RecordId(1, 2));

        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // Attempts to grow the records should fail.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 101);
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 51);

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_NOT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize(), false, nullptr));
        ASSERT_NOT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize(), false, nullptr));
    }

    // Attempts to shrink the records should also fail.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 99);
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 49);

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_NOT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize(), false, nullptr));
        ASSERT_NOT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize(), false, nullptr));
    }

    // Changing the contents of the records without changing their size should succeed.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 100, 'y');
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 50, 'z');

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize(), false, nullptr));
        ASSERT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize(), false, nullptr));
        wuow.commit();

        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }
}

// Insert multiple records and truncate the oplog using RecordStore::truncate(). The operation
// should leave no stones, including the partially filled one.
TEST(WiredTigerRecordStoreTest, OplogStones_Truncate) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper->newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 50), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 50), RecordId(1, 2));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 50), RecordId(1, 3));

        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(150, rs->dataSize(opCtx.get()));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->truncate(opCtx.get()));
        wuow.commit();

        ASSERT_EQ(0, rs->dataSize(opCtx.get()));
        ASSERT_EQ(0, rs->numRecords(opCtx.get()));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
}

// Insert multiple records, truncate the oplog using RecordStore::cappedTruncateAfter(), and
// verify that the metadata for each stone is updated. If a full stone is partially truncated, then
// it should become the stone currently being filled.
TEST(WiredTigerRecordStoreTest, OplogStones_CappedTruncateAfter) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper->newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(1000);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 400), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 800), RecordId(1, 2));

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 200), RecordId(1, 3));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 4), 250), RecordId(1, 4));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 5), 300), RecordId(1, 5));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 6), 350), RecordId(1, 6));

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 7), 50), RecordId(1, 7));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 8), 100), RecordId(1, 8));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 9), 150), RecordId(1, 9));

        ASSERT_EQ(9, rs->numRecords(opCtx.get()));
        ASSERT_EQ(2600, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(3, oplogStones->currentRecords());
        ASSERT_EQ(300, oplogStones->currentBytes());
    }

    // Make sure all are visible.
    rs->waitForAllEarlierOplogWritesToBeVisible(harnessHelper->newOperationContext().get());

    // Truncate data using an inclusive RecordId that exists inside the stone currently being
    // filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 8), true);

        ASSERT_EQ(7, rs->numRecords(opCtx.get()));
        ASSERT_EQ(2350, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // Truncate data using an inclusive RecordId that refers to the 'lastRecord' of a full stone.
    // The stone should become the one currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 6), true);

        ASSERT_EQ(5, rs->numRecords(opCtx.get()));
        ASSERT_EQ(1950, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(3, oplogStones->currentRecords());
        ASSERT_EQ(750, oplogStones->currentBytes());
    }

    // Truncate data using a non-inclusive RecordId that exists inside the stone currently being
    // filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 3), false);

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(1400, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(200, oplogStones->currentBytes());
    }

    // Truncate data using a non-inclusive RecordId that refers to the 'lastRecord' of a full stone.
    // The stone should remain intact.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 2), false);

        ASSERT_EQ(2, rs->numRecords(opCtx.get()));
        ASSERT_EQ(1200, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    // Truncate data using a non-inclusive RecordId that exists inside a full stone. The stone
    // should become the one currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        rs->cappedTruncateAfter(opCtx.get(), RecordId(1, 1), false);

        ASSERT_EQ(1, rs->numRecords(opCtx.get()));
        ASSERT_EQ(400, rs->dataSize(opCtx.get()));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(400, oplogStones->currentBytes());
    }
}

// Verify that oplog stones are reclaimed when cappedMaxSize is exceeded.
TEST(WiredTigerRecordStoreTest, OplogStones_ReclaimStones) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper->newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_OK(wtrs->updateCappedSize(opCtx.get(), 230U));
    }

    oplogStones->setMinBytesPerStone(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 100), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 110), RecordId(1, 2));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 120), RecordId(1, 3));

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(330, rs->dataSize(opCtx.get()));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    // Fail to truncate stone when cappedMaxSize is exceeded, but the persisted timestamp is
    // before the truncation point (i.e: leaves a gap that replication recovery would rely on).
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtrs->reclaimOplog(opCtx.get(), Timestamp(1, 0));

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(330, rs->dataSize(opCtx.get()));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    // Truncate a stone when cappedMaxSize is exceeded.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtrs->reclaimOplog(opCtx.get(), Timestamp(1, 3));

        ASSERT_EQ(2, rs->numRecords(opCtx.get()));
        ASSERT_EQ(230, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 4), 130), RecordId(1, 4));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 5), 140), RecordId(1, 5));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 6), 50), RecordId(1, 6));

        ASSERT_EQ(5, rs->numRecords(opCtx.get()));
        ASSERT_EQ(550, rs->dataSize(opCtx.get()));
        ASSERT_EQ(4U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // Truncate multiple stones if necessary.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtrs->reclaimOplog(opCtx.get(), Timestamp(1, 6));

        ASSERT_EQ(2, rs->numRecords(opCtx.get()));
        ASSERT_EQ(190, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // No-op if dataSize <= cappedMaxSize.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        wtrs->reclaimOplog(opCtx.get(), Timestamp(1, 6));

        ASSERT_EQ(2, rs->numRecords(opCtx.get()));
        ASSERT_EQ(190, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }
}

// Verify that an oplog stone isn't created if it would cause the logical representation of the
// records to not be in increasing order.
TEST(WiredTigerRecordStoreTest, OplogStones_AscendingOrder) {
    std::unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper->newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(2, 2), 50), RecordId(2, 2));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());

        // Inserting a record that has a smaller RecordId than the previously inserted record should
        // be able to create a new stone when no stones already exist.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(2, 1), 50), RecordId(2, 1));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());

        // However, inserting a record that has a smaller RecordId than most recently created
        // stone's last record shouldn't cause a new stone to be created, even if the size of the
        // inserted record exceeds 'minBytesPerStone'.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 100), RecordId(1, 1));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(100, oplogStones->currentBytes());

        // Inserting a record that has a larger RecordId than the most recently created stone's last
        // record should then cause a new stone to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(2, 3), 50), RecordId(2, 3));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
}

}  // namespace
}  // namespace mongo
