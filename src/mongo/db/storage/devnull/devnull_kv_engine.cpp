
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

#include "mongo/db/storage/devnull/devnull_kv_engine.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_record_store.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class EmptyRecordCursor final : public SeekableRecordCursor {
public:
    boost::optional<Record> next() final {
        return {};
    }
    boost::optional<Record> seekExact(const RecordId& id) final {
        return {};
    }
    void save() final {}
    bool restore() final {
        return true;
    }
    void detachFromOperationContext() final {}
    void reattachToOperationContext(OperationContext* opCtx) final {}
};

class DevNullRecordStore : public RecordStore {
public:
    DevNullRecordStore(StringData ns, const CollectionOptions& options)
        : RecordStore(ns), _options(options) {
        _numInserts = 0;
        _dummy = BSON("_id" << 1);
    }

    virtual const char* name() const {
        return "devnull";
    }

    const std::string& getIdent() const override {
        MONGO_UNREACHABLE;
    }

    virtual void setCappedCallback(CappedCallback*) {}

    virtual long long dataSize(OperationContext* opCtx) const {
        return 0;
    }

    virtual long long numRecords(OperationContext* opCtx) const {
        return 0;
    }

    virtual bool isCapped() const {
        return _options.capped;
    }

    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = NULL,
                                int infoLevel = 0) const {
        return 0;
    }

    virtual RecordData dataFor(OperationContext* opCtx, const RecordId& loc) const {
        return RecordData(_dummy.objdata(), _dummy.objsize());
    }

    virtual bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* rd) const {
        return false;
    }

    virtual void deleteRecord(OperationContext* opCtx, const RecordId& dl) {}

    virtual StatusWith<RecordId> insertRecord(
        OperationContext* opCtx, const char* data, int len, Timestamp, bool enforceQuota) {
        _numInserts++;
        return StatusWith<RecordId>(RecordId(6, 4));
    }

    virtual Status insertRecordsWithDocWriter(OperationContext* opCtx,
                                              const DocWriter* const* docs,
                                              const Timestamp*,
                                              size_t nDocs,
                                              RecordId* idsOut) {
        _numInserts += nDocs;
        if (idsOut) {
            for (size_t i = 0; i < nDocs; i++) {
                idsOut[i] = RecordId(6, 4);
            }
        }
        return Status::OK();
    }

    virtual Status updateRecord(OperationContext* opCtx,
                                const RecordId& oldLocation,
                                const char* data,
                                int len,
                                bool enforceQuota,
                                UpdateNotifier* notifier) {
        return Status::OK();
    }

    virtual bool updateWithDamagesSupported() const {
        return false;
    }

    virtual StatusWith<RecordData> updateWithDamages(OperationContext* opCtx,
                                                     const RecordId& loc,
                                                     const RecordData& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages) {
        MONGO_UNREACHABLE;
    }


    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const final {
        return stdx::make_unique<EmptyRecordCursor>();
    }

    virtual Status truncate(OperationContext* opCtx) {
        return Status::OK();
    }

    virtual void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) {}

    virtual Status validate(OperationContext* opCtx,
                            ValidateCmdLevel level,
                            ValidateAdaptor* adaptor,
                            ValidateResults* results,
                            BSONObjBuilder* output) {
        return Status::OK();
    }

    virtual void appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* result,
                                   double scale) const {
        result->appendNumber("numInserts", _numInserts);
    }

    virtual Status touch(OperationContext* opCtx, BSONObjBuilder* output) const {
        return Status::OK();
    }

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const override {}

    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize) {}

private:
    CollectionOptions _options;
    long long _numInserts;
    BSONObj _dummy;
};

class DevNullSortedDataBuilderInterface : public SortedDataBuilderInterface {
    MONGO_DISALLOW_COPYING(DevNullSortedDataBuilderInterface);

public:
    DevNullSortedDataBuilderInterface() {}

    virtual Status addKey(const BSONObj& key, const RecordId& loc) {
        return Status::OK();
    }
};

class DevNullSortedDataInterface : public SortedDataInterface {
public:
    virtual ~DevNullSortedDataInterface() {}

    virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* opCtx, bool dupsAllowed) {
        return new DevNullSortedDataBuilderInterface();
    }

    virtual Status insert(OperationContext* opCtx,
                          const BSONObj& key,
                          const RecordId& loc,
                          bool dupsAllowed) {
        return Status::OK();
    }

    virtual void unindex(OperationContext* opCtx,
                         const BSONObj& key,
                         const RecordId& loc,
                         bool dupsAllowed) {}

    virtual Status dupKeyCheck(OperationContext* opCtx, const BSONObj& key, const RecordId& loc) {
        return Status::OK();
    }

    virtual void fullValidate(OperationContext* opCtx,
                              long long* numKeysOut,
                              ValidateResults* fullResults) const {}

    virtual bool appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* output,
                                   double scale) const {
        return false;
    }

    virtual long long getSpaceUsedBytes(OperationContext* opCtx) const {
        return 0;
    }

    virtual bool isEmpty(OperationContext* opCtx) {
        return true;
    }

    virtual std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                                   bool isForward) const {
        return {};
    }

    virtual Status initAsEmpty(OperationContext* opCtx) {
        return Status::OK();
    }
};


std::unique_ptr<RecordStore> DevNullKVEngine::getRecordStore(OperationContext* opCtx,
                                                             StringData ns,
                                                             StringData ident,
                                                             const CollectionOptions& options) {
    if (ident == "_mdb_catalog") {
        return stdx::make_unique<EphemeralForTestRecordStore>(ns, &_catalogInfo);
    }
    return stdx::make_unique<DevNullRecordStore>(ns, options);
}

SortedDataInterface* DevNullKVEngine::getSortedDataInterface(OperationContext* opCtx,
                                                             StringData ident,
                                                             const IndexDescriptor* desc) {
    return new DevNullSortedDataInterface();
}
}
