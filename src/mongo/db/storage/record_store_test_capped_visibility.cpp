
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

#include <memory>

#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/unowned_ptr.h"


namespace mongo {
namespace {

RecordId doInsert(unowned_ptr<OperationContext> opCtx, unowned_ptr<RecordStore> rs) {
    static char zeros[16];
    return uassertStatusOK(rs->insertRecord(opCtx, zeros, sizeof(zeros), Timestamp(), false));
}

// macro to keep assert line numbers correct.
#define ASSERT_ID_EQ(EXPR, ID)                        \
    [](boost::optional<Record> record, RecordId id) { \
        ASSERT(record);                               \
        ASSERT_EQ(record->id, id);                    \
    }((EXPR), (ID));

TEST(RecordStore_CappedVisibility, EmptyInitialState) {
    const auto harness = newRecordStoreHarnessHelper();
    if (!harness->supportsDocLocking())
        return;

    auto rs = harness->newCappedRecordStore();

    auto longLivedClient = harness->serviceContext()->makeClient("longLived");
    auto longLivedOp = harness->newOperationContext(longLivedClient.get());
    WriteUnitOfWork longLivedWuow(longLivedOp.get());

    // Collection is really empty.
    ASSERT(!rs->getCursor(longLivedOp.get(), true)->next());
    ASSERT(!rs->getCursor(longLivedOp.get(), false)->next());

    RecordId lowestHiddenId = doInsert(longLivedOp, rs);
    RecordId otherId;
    {
        auto opCtx = harness->newOperationContext();
        WriteUnitOfWork wuow(opCtx.get());

        // Can't see uncommitted write from other operation.
        ASSERT(!rs->getCursor(opCtx.get())->seekExact(lowestHiddenId));

        ASSERT(!rs->getCursor(opCtx.get(), true)->next());
        ASSERT(!rs->getCursor(opCtx.get(), false)->next());

        otherId = doInsert(opCtx, rs);

        // Can read own writes.
        ASSERT_ID_EQ(rs->getCursor(opCtx.get(), true)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(opCtx.get(), false)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(opCtx.get())->seekExact(otherId), otherId);

        wuow.commit();
    }

    // longLivedOp is still on old snapshot so it can't see otherId yet.
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), true)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), false)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT(!rs->getCursor(longLivedOp.get())->seekExact(otherId));

    // Make all documents visible and let longLivedOp get a new snapshot.
    longLivedWuow.commit();

    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), true)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), false)->next(), otherId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get())->seekExact(otherId), otherId);
}

TEST(RecordStore_CappedVisibility, NonEmptyInitialState) {
    const auto harness = newRecordStoreHarnessHelper();
    if (!harness->supportsDocLocking())
        return;

    auto rs = harness->newCappedRecordStore();

    auto longLivedClient = harness->serviceContext()->makeClient("longLived");
    auto longLivedOp = harness->newOperationContext(longLivedClient.get());

    RecordId initialId;
    {
        WriteUnitOfWork wuow(longLivedOp.get());
        initialId = doInsert(longLivedOp, rs);
        wuow.commit();
    }

    WriteUnitOfWork longLivedWuow(longLivedOp.get());

    // Can see initial doc.
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), false)->next(), initialId);

    RecordId lowestHiddenId = doInsert(longLivedOp, rs);

    // Collection still looks like it only has a single doc to iteration but not seekExact.
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), false)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get())->seekExact(initialId), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get())->seekExact(lowestHiddenId), lowestHiddenId);

    RecordId otherId;
    {
        auto opCtx = harness->newOperationContext();
        WriteUnitOfWork wuow(opCtx.get());

        // Can only see committed writes from other operation.
        ASSERT_ID_EQ(rs->getCursor(opCtx.get())->seekExact(initialId), initialId);
        ASSERT(!rs->getCursor(opCtx.get())->seekExact(lowestHiddenId));

        ASSERT_ID_EQ(rs->getCursor(opCtx.get(), true)->next(), initialId);
        ASSERT_ID_EQ(rs->getCursor(opCtx.get(), false)->next(), initialId);

        otherId = doInsert(opCtx, rs);

        ASSERT_ID_EQ(rs->getCursor(opCtx.get(), true)->next(), initialId);
        ASSERT_ID_EQ(rs->getCursor(opCtx.get(), false)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(opCtx.get())->seekExact(otherId), otherId);

        wuow.commit();

        ASSERT_ID_EQ(rs->getCursor(opCtx.get(), true)->next(), initialId);
        ASSERT_ID_EQ(rs->getCursor(opCtx.get(), false)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(opCtx.get())->seekExact(otherId), otherId);
        ASSERT(!rs->getCursor(opCtx.get())->seekExact(lowestHiddenId));
    }

    // longLivedOp is still on old snapshot so it can't see otherId yet.
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), false)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT(!rs->getCursor(longLivedOp.get())->seekExact(otherId));

    // This makes all documents visible and lets longLivedOp get a new snapshot.
    longLivedWuow.commit();

    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get(), false)->next(), otherId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get())->seekExact(initialId), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOp.get())->seekExact(otherId), otherId);
}

}  // namespace
}  // namespace mongo
