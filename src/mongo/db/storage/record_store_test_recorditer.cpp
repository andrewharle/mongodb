// record_store_test_recorditer.cpp


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

#include "mongo/db/storage/record_store_test_harness.h"

#include <algorithm>

#include "mongo/bson/util/builder.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::unique_ptr;
using std::string;
using std::stringstream;

// Insert multiple records and iterate through them in the forward direction.
// When curr() or getNext() is called on an iterator positioned at EOF,
// the iterator returns RecordId() and stays at EOF.
TEST(RecordStoreTestHarness, IterateOverMultipleRecords) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            stringstream ss;
            ss << "record " << i;
            string data = ss.str();

            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            locs[i] = res.getValue();
            datas[i] = data;
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, rs->numRecords(opCtx.get()));
    }

    std::sort(locs, locs + nToInsert);  // inserted records may not be in RecordId order
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get());
        for (int i = 0; i < nToInsert; i++) {
            const auto record = cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }
        ASSERT(!cursor->next());
    }
}

// Insert multiple records and iterate through them in the reverse direction.
// When curr() or getNext() is called on an iterator positioned at EOF,
// the iterator returns RecordId() and stays at EOF.
TEST(RecordStoreTestHarness, IterateOverMultipleRecordsReversed) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            stringstream ss;
            ss << "record " << i;
            string data = ss.str();

            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            locs[i] = res.getValue();
            datas[i] = data;
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, rs->numRecords(opCtx.get()));
    }

    std::sort(locs, locs + nToInsert);  // inserted records may not be in RecordId order
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        auto cursor = rs->getCursor(opCtx.get(), false);
        for (int i = nToInsert - 1; i >= 0; i--) {
            const auto record = cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }
        ASSERT(!cursor->next());
    }
}

// Insert multiple records and try to create a forward iterator
// starting at an interior position.
TEST(RecordStoreTestHarness, IterateStartFromMiddle) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            stringstream ss;
            ss << "record " << i;
            string data = ss.str();

            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            locs[i] = res.getValue();
            datas[i] = data;
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, rs->numRecords(opCtx.get()));
    }

    std::sort(locs, locs + nToInsert);  // inserted records may not be in RecordId order
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        int start = nToInsert / 2;
        auto cursor = rs->getCursor(opCtx.get());
        for (int i = start; i < nToInsert; i++) {
            const auto record = (i == start) ? cursor->seekExact(locs[i]) : cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }
        ASSERT(!cursor->next());
    }
}

// Insert multiple records and try to create a reverse iterator
// starting at an interior position.
TEST(RecordStoreTestHarness, IterateStartFromMiddleReversed) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            stringstream ss;
            ss << "record " << i;
            string data = ss.str();

            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            locs[i] = res.getValue();
            datas[i] = data;
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, rs->numRecords(opCtx.get()));
    }

    std::sort(locs, locs + nToInsert);  // inserted records may not be in RecordId order
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        int start = nToInsert / 2;
        auto cursor = rs->getCursor(opCtx.get(), false);
        for (int i = start; i >= 0; i--) {
            const auto record = (i == start) ? cursor->seekExact(locs[i]) : cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }
        ASSERT(!cursor->next());
    }
}

// Insert several records, and iterate to the end. Ensure that the record iterator
// is EOF. Add an additional record, saving and restoring the iterator state, and check
// that the iterator remains EOF.
TEST(RecordStoreTestHarness, RecordIteratorEOF) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            StringBuilder sb;
            sb << "record " << i;
            string data = sb.str();

            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            locs[i] = res.getValue();
            datas[i] = data;
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        // Get a forward iterator starting at the beginning of the record store.
        auto cursor = rs->getCursor(opCtx.get());

        // Iterate, checking EOF along the way.
        for (int i = 0; i < nToInsert; i++) {
            const auto record = cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }
        ASSERT(!cursor->next());

        // Add a record and ensure we're still EOF.
        cursor->save();

        StringBuilder sb;
        sb << "record " << nToInsert + 1;
        string data = sb.str();

        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res =
            rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp(), false);
        ASSERT_OK(res.getStatus());
        uow.commit();

        ASSERT(cursor->restore());

        // Iterator should still be EOF.
        ASSERT(!cursor->next());
        ASSERT(!cursor->next());
    }
}

// Test calling save and restore after each call to next
TEST(RecordStoreTestHarness, RecordIteratorSaveRestore) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    const int nToInsert = 10;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            StringBuilder sb;
            sb << "record " << i;
            string data = sb.str();

            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            locs[i] = res.getValue();
            datas[i] = data;
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        // Get a forward iterator starting at the beginning of the record store.
        auto cursor = rs->getCursor(opCtx.get());

        // Iterate, checking EOF along the way.
        for (int i = 0; i < nToInsert; i++) {
            cursor->save();
            cursor->save();  // It is legal to save twice in a row.
            cursor->restore();

            const auto record = cursor->next();
            ASSERT(record);
            ASSERT_EQUALS(locs[i], record->id);
            ASSERT_EQUALS(datas[i], record->data.data());
        }

        cursor->save();
        cursor->save();  // It is legal to save twice in a row.
        cursor->restore();

        ASSERT(!cursor->next());
    }
}

// Insert two records, and iterate a cursor to EOF. Seek the same cursor to the first and ensure
// that next() returns the second record.
TEST(RecordStoreTestHarness, SeekAfterEofAndContinue) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

    const int nToInsert = 2;
    RecordId locs[nToInsert];
    std::string datas[nToInsert];
    for (int i = 0; i < nToInsert; i++) {
        StringBuilder sb;
        sb << "record " << i;
        string data = sb.str();

        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res =
            rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, Timestamp(), false);
        ASSERT_OK(res.getStatus());
        locs[i] = res.getValue();
        datas[i] = data;
        uow.commit();
    }


    // Get a forward iterator starting at the beginning of the record store.
    auto cursor = rs->getCursor(opCtx.get());

    // Iterate, checking EOF along the way.
    for (int i = 0; i < nToInsert; i++) {
        const auto record = cursor->next();
        ASSERT(record);
        ASSERT_EQUALS(locs[i], record->id);
        ASSERT_EQUALS(datas[i], record->data.data());
    }
    ASSERT(!cursor->next());

    {
        const auto record = cursor->seekExact(locs[0]);
        ASSERT(record);
        ASSERT_EQUALS(locs[0], record->id);
        ASSERT_EQUALS(datas[0], record->data.data());
    }

    {
        const auto record = cursor->next();
        ASSERT(record);
        ASSERT_EQUALS(locs[1], record->id);
        ASSERT_EQUALS(datas[1], record->data.data());
    }

    ASSERT(!cursor->next());
}

}  // namespace
}  // namespace mongo
