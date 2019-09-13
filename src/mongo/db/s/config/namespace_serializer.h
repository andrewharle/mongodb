
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

#include <map>
#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class OperationContext;

class NamespaceSerializer {
    MONGO_DISALLOW_COPYING(NamespaceSerializer);

public:
    class ScopedLock {
    public:
        ~ScopedLock();

    private:
        friend class NamespaceSerializer;
        ScopedLock(StringData ns, NamespaceSerializer& nsSerializer);

        std::string _ns;
        NamespaceSerializer& _nsSerializer;
    };

    NamespaceSerializer();

    ScopedLock lock(OperationContext* opCtx, StringData ns);

private:
    struct NSLock {
        stdx::condition_variable cvLocked;
        int numWaiting = 1;
        bool isInProgress = true;
    };

    stdx::mutex _mutex;
    StringMap<std::shared_ptr<NSLock>> _inProgressMap;
};

}  // namespace mongo
