// sorted_data_interface_test_spaceused.cpp


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

#include "mongo/db/storage/sorted_data_interface_test_harness.h"

#include <memory>

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Verify that an empty index takes up no space.
TEST(SortedDataInterface, GetSpaceUsedBytesEmpty) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    // SERVER-15416 mmapv1 test harness does not use SimpleRecordStoreV1 as its record store
    //              and HeapRecordStoreBtree::dataSize does not have an actual implementation
    // {
    //     const ServiceContext::UniqueOperationContext opCtx( harnessHelper->newOperationContext()
    //     );
    //     ASSERT( sorted->getSpaceUsedBytes( opCtx.get() ) == 0 );
    // }
}

// Verify that a nonempty index takes up some space.
TEST(SortedDataInterface, GetSpaceUsedBytesNonEmpty) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            BSONObj key = BSON("" << i);
            RecordId loc(42, i * 2);
            ASSERT_OK(sorted->insert(opCtx.get(), key, loc, true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx.get()));
    }

    // SERVER-15416 mmapv1 test harness does not use SimpleRecordStoreV1 as its record store
    //              and HeapRecordStoreBtree::dataSize does not have an actual implementation
    // long long spaceUsedBytes;
    // {
    //     const ServiceContext::UniqueOperationContext opCtx( harnessHelper->newOperationContext()
    //     );
    //     spaceUsedBytes = sorted->getSpaceUsedBytes( opCtx.get() );
    //     ASSERT( spaceUsedBytes > 0 );
    // }

    // {
    //     // getSpaceUsedBytes() returns the same value when called multiple times
    //     // and there were not interleaved write operations.
    //     const ServiceContext::UniqueOperationContext opCtx( harnessHelper->newOperationContext()
    //     );
    //     ASSERT_EQUALS( spaceUsedBytes, sorted->getSpaceUsedBytes( opCtx.get() ) );
    //     ASSERT_EQUALS( spaceUsedBytes, sorted->getSpaceUsedBytes( opCtx.get() ) );
    // }
}

}  // namespace
}  // namespace mongo
