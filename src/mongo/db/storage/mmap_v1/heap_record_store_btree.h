// heap_record_store_btree.h


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

#include <boost/shared_array.hpp>
#include <map>

#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"

namespace mongo {

/**
 * A RecordStore that stores all data on the heap. This implementation contains only the
 * functionality necessary to test btree.
 */
class HeapRecordStoreBtree : public RecordStore {
    struct MmapV1RecordHeader;

public:
    const std::string& getIdent() const override {
        MONGO_UNREACHABLE;
    }

    // RecordId(0,0) isn't valid for records.
    explicit HeapRecordStoreBtree(StringData ns) : RecordStore(ns), _nextId(1) {}

    virtual RecordData dataFor(OperationContext* opCtx, const RecordId& loc) const;

    virtual bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* out) const;

    virtual void deleteRecord(OperationContext* opCtx, const RecordId& dl);

    virtual StatusWith<RecordId> insertRecord(
        OperationContext* opCtx, const char* data, int len, Timestamp, bool enforceQuota);

    virtual Status insertRecordsWithDocWriter(OperationContext* opCtx,
                                              const DocWriter* const* docs,
                                              const Timestamp*,
                                              size_t nDocs,
                                              RecordId* idsOut);

    virtual long long numRecords(OperationContext* opCtx) const {
        return _records.size();
    }

    virtual Status touch(OperationContext* opCtx, BSONObjBuilder* output) const;

    // public methods below here are not necessary to test btree, and will crash when called.

    // ------------------------------

    virtual Status updateRecord(OperationContext* opCtx,
                                const RecordId& oldLocation,
                                const char* data,
                                int len,
                                bool enforceQuota,
                                UpdateNotifier* notifier) {
        MONGO_UNREACHABLE;
    }

    virtual bool updateWithDamagesSupported() const {
        return true;
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
        MONGO_UNREACHABLE;
    }


    virtual Status truncate(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    virtual void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) {
        MONGO_UNREACHABLE;
    }

    virtual bool compactSupported() const {
        MONGO_UNREACHABLE;
    }

    virtual Status validate(OperationContext* opCtx,
                            ValidateCmdLevel level,
                            ValidateAdaptor* adaptor,
                            ValidateResults* results,
                            BSONObjBuilder* output) {
        MONGO_UNREACHABLE;
    }

    virtual void appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* result,
                                   double scale) const {
        MONGO_UNREACHABLE;
    }

    virtual void increaseStorageSize(OperationContext* opCtx, int size, bool enforceQuota) {
        MONGO_UNREACHABLE;
    }

    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = NULL,
                                int infoLevel = 0) const {
        MONGO_UNREACHABLE;
    }

    virtual long long dataSize(OperationContext* opCtx) const {
        MONGO_UNREACHABLE;
    }

    virtual MmapV1RecordHeader* recordFor(const RecordId& loc) const {
        MONGO_UNREACHABLE;
    }

    virtual bool isCapped() const {
        MONGO_UNREACHABLE;
    }

    virtual const char* name() const {
        MONGO_UNREACHABLE;
    }

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const override {
        MONGO_UNREACHABLE;
    }

    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize) {
        MONGO_UNREACHABLE;
    }
    // more things that we actually care about below

private:
    struct MmapV1RecordHeader {
        MmapV1RecordHeader() : dataSize(-1), data() {}
        explicit MmapV1RecordHeader(int size) : dataSize(size), data(new char[size]) {}

        int dataSize;
        boost::shared_array<char> data;
    };

    RecordId allocateLoc();

    typedef std::map<RecordId, HeapRecordStoreBtree::MmapV1RecordHeader> Records;
    Records _records;
    int64_t _nextId;
};

/**
 * A RecoveryUnit for HeapRecordStoreBtree, this is for testing btree only.
 */
class HeapRecordStoreBtreeRecoveryUnit : public RecoveryUnit {
public:
    void beginUnitOfWork(OperationContext* opCtx) final{};
    void commitUnitOfWork() final;
    void abortUnitOfWork() final;

    virtual bool waitUntilDurable() {
        return true;
    }

    virtual void abandonSnapshot() {}

    virtual void registerChange(Change* change) {
        change->commit(boost::none);
        delete change;
    }

    virtual void* writingPtr(void* data, size_t len);

    virtual void setRollbackWritesDisabled() {}

    virtual SnapshotId getSnapshotId() const {
        return SnapshotId();
    }

    virtual void setOrderedCommit(bool orderedCommit) {}

    // -----------------------

    void notifyInsert(HeapRecordStoreBtree* rs, const RecordId& loc);
    static void notifyInsert(OperationContext* ctx, HeapRecordStoreBtree* rs, const RecordId& loc);

private:
    struct InsertEntry {
        HeapRecordStoreBtree* rs;
        RecordId loc;
    };
    std::vector<InsertEntry> _insertions;

    struct ModEntry {
        void* data;
        size_t len;
        boost::shared_array<char> old;
    };
    std::vector<ModEntry> _mods;
};

}  // namespace mongo
