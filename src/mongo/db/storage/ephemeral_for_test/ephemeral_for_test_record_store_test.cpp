
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

#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_record_store.h"

#include "mongo/base/init.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class EphemeralForTestHarnessHelper final : public RecordStoreHarnessHelper {
public:
    EphemeralForTestHarnessHelper() {}

    virtual std::unique_ptr<RecordStore> newNonCappedRecordStore() {
        return newNonCappedRecordStore("a.b");
    }

    virtual std::unique_ptr<RecordStore> newNonCappedRecordStore(const std::string& ns) {
        return stdx::make_unique<EphemeralForTestRecordStore>(ns, &data);
    }

    virtual std::unique_ptr<RecordStore> newCappedRecordStore(int64_t cappedSizeBytes,
                                                              int64_t cappedMaxDocs) {
        return newCappedRecordStore("a.b", cappedSizeBytes, cappedMaxDocs);
    }

    virtual std::unique_ptr<RecordStore> newCappedRecordStore(const std::string& ns,
                                                              int64_t cappedSizeBytes,
                                                              int64_t cappedMaxDocs) final {
        return stdx::make_unique<EphemeralForTestRecordStore>(
            ns, &data, true, cappedSizeBytes, cappedMaxDocs);
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() final {
        return stdx::make_unique<EphemeralForTestRecoveryUnit>();
    }

    bool supportsDocLocking() final {
        return false;
    }

    std::shared_ptr<void> data;
};

std::unique_ptr<HarnessHelper> makeHarnessHelper() {
    return stdx::make_unique<EphemeralForTestHarnessHelper>();
}

MONGO_INITIALIZER(RegisterHarnessFactory)(InitializerContext* const) {
    mongo::registerHarnessHelperFactory(makeHarnessHelper);
    return Status::OK();
}
}  // namespace
}  // namespace mongo
