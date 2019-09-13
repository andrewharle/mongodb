// record_store_test_harness.h


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

#pragma once

#include <cstdint>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/test_harness_helper.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class RecordStore;
class RecoveryUnit;

class RecordStoreHarnessHelper : public HarnessHelper {
public:
    virtual std::unique_ptr<RecordStore> newNonCappedRecordStore() = 0;

    virtual std::unique_ptr<RecordStore> newNonCappedRecordStore(const std::string& ns) = 0;

    static const int64_t kDefaultCapedSizeBytes = 16 * 1024 * 1024;
    virtual std::unique_ptr<RecordStore> newCappedRecordStore(
        int64_t cappedSizeBytes = kDefaultCapedSizeBytes, int64_t cappedMaxDocs = -1) = 0;

    virtual std::unique_ptr<RecordStore> newCappedRecordStore(const std::string& ns,
                                                              int64_t cappedSizeBytes,
                                                              int64_t cappedMaxDocs) = 0;

    /**
     * Currently this requires that it is possible to have two independent open write operations
     * at the same time one the same thread (with separate Clients, OperationContexts, and
     * RecoveryUnits).
     */
    virtual bool supportsDocLocking() = 0;
};

inline std::unique_ptr<RecordStoreHarnessHelper> newRecordStoreHarnessHelper() {
    return dynamic_ptr_cast<RecordStoreHarnessHelper>(newHarnessHelper());
}
}  // namespace mongo
