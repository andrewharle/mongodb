// ephemeral_for_test_engine.cpp


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

#include <memory>

#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_engine.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_btree_impl.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_record_store.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/stdx/memory.h"

namespace mongo {

RecoveryUnit* EphemeralForTestEngine::newRecoveryUnit() {
    return new EphemeralForTestRecoveryUnit([this]() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        JournalListener::Token token = _journalListener->getToken();
        _journalListener->onDurable(token);
    });
}

Status EphemeralForTestEngine::createRecordStore(OperationContext* opCtx,
                                                 StringData ns,
                                                 StringData ident,
                                                 const CollectionOptions& options) {
    // Register the ident in the `_dataMap` (for `getAllIdents`). Remainder of work done in
    // `getRecordStore`.
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _dataMap[ident] = {};
    return Status::OK();
}

std::unique_ptr<RecordStore> EphemeralForTestEngine::getRecordStore(
    OperationContext* opCtx, StringData ns, StringData ident, const CollectionOptions& options) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (options.capped) {
        return stdx::make_unique<EphemeralForTestRecordStore>(
            ns,
            &_dataMap[ident],
            true,
            options.cappedSize ? options.cappedSize : 4096,
            options.cappedMaxDocs ? options.cappedMaxDocs : -1);
    } else {
        return stdx::make_unique<EphemeralForTestRecordStore>(ns, &_dataMap[ident]);
    }
}

Status EphemeralForTestEngine::createSortedDataInterface(OperationContext* opCtx,
                                                         StringData ident,
                                                         const IndexDescriptor* desc) {
    // Register the ident in `_dataMap` (for `getAllIdents`). Remainder of work done in
    // `getSortedDataInterface`.
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _dataMap[ident] = {};
    return Status::OK();
}

SortedDataInterface* EphemeralForTestEngine::getSortedDataInterface(OperationContext* opCtx,
                                                                    StringData ident,
                                                                    const IndexDescriptor* desc) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return getEphemeralForTestBtreeImpl(
        Ordering::make(desc->keyPattern()), desc->unique(), &_dataMap[ident]);
}

Status EphemeralForTestEngine::dropIdent(OperationContext* opCtx, StringData ident) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _dataMap.erase(ident);
    return Status::OK();
}

int64_t EphemeralForTestEngine::getIdentSize(OperationContext* opCtx, StringData ident) {
    return 1;
}

std::vector<std::string> EphemeralForTestEngine::getAllIdents(OperationContext* opCtx) const {
    std::vector<std::string> all;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        for (DataMap::const_iterator it = _dataMap.begin(); it != _dataMap.end(); ++it) {
            all.push_back(it->first);
        }
    }
    return all;
}
}
