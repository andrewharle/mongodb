
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"

namespace mongo {

/**
 * WT_CONNECTION::reconfigure get/setParameter support
 */
class WiredTigerEngineRuntimeConfigParameter : public ServerParameter {
    MONGO_DISALLOW_COPYING(WiredTigerEngineRuntimeConfigParameter);

public:
    explicit WiredTigerEngineRuntimeConfigParameter(WiredTigerKVEngine* engine);

    /**
     * Appends the last value that was successfully assigned via a call to `set` or
     * `setFromString`. To conclude what options WiredTiger is running with, consult what MongoDB
     * logged at startup when making the `wiredtiger_open` call.
     */
    virtual void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name);
    virtual Status set(const BSONElement& newValueElement);

    virtual Status setFromString(const std::string& str);

private:
    WiredTigerKVEngine* _engine;
    // This parameter can only be modified at runtime via `setParameter`. This string always
    // starts out as the empty string.
    std::string _currentValue;
};

class WiredTigerMaxCacheOverflowSizeGBParameter : public ServerParameter {
    MONGO_DISALLOW_COPYING(WiredTigerMaxCacheOverflowSizeGBParameter);

public:
    WiredTigerMaxCacheOverflowSizeGBParameter(WiredTigerKVEngine* engine, double value);

    virtual void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name);
    virtual Status set(const BSONElement& newValueElement);
    virtual Status setFromString(const std::string& str);

    std::pair<double, WiredTigerKVEngine*> _data;
};
}
