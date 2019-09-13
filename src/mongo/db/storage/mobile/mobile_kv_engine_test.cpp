
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

#include "mongo/base/init.h"

#include "mongo/db/storage/kv/kv_engine_test_harness.h"
#include "mongo/db/storage/mobile/mobile_kv_engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"

namespace mongo {
namespace {

class MobileKVHarnessHelper : public KVHarnessHelper {
public:
    MobileKVHarnessHelper() : _dbPath("mobile_kv_engine_harness"), _mobileDurabilityLevel(1) {
        _engine = stdx::make_unique<MobileKVEngine>(_dbPath.path(), _mobileDurabilityLevel);
    }

    virtual KVEngine* restartEngine() {
        _engine.reset(new MobileKVEngine(_dbPath.path(), _mobileDurabilityLevel));
        return _engine.get();
    }

    virtual KVEngine* getEngine() {
        return _engine.get();
    }

private:
    std::unique_ptr<MobileKVEngine> _engine;
    unittest::TempDir _dbPath;
    std::int32_t _mobileDurabilityLevel;
};

std::unique_ptr<KVHarnessHelper> makeHelper() {
    return stdx::make_unique<MobileKVHarnessHelper>();
}

MONGO_INITIALIZER(RegisterKVHarnessFactory)(InitializerContext*) {
    KVHarnessHelper::registerFactory(makeHelper);
    return Status::OK();
}

}  // namespace
}  // namespace mongo
