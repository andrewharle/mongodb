// ephemeral_for_test_record_store.h


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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

/**
 * A RecordStore that stores all data in-memory.
 *
 * @param cappedMaxSize - required if isCapped. limit uses dataSize() in this impl.
 */
class EphemeralForTestRecordStore : public RecordStore {
public:
    explicit EphemeralForTestRecordStore(StringData ns,
                                         std::shared_ptr<void>* dataInOut,
                                         bool isCapped = false,
                                         int64_t cappedMaxSize = -1,
                                         int64_t cappedMaxDocs = -1,
                                         CappedCallback* cappedCallback = nullptr);

    virtual const char* name() const;

    const std::string& getIdent() const override {
        return ns();
    }

    virtual RecordData dataFor(OperationContext* opCtx, const RecordId& loc) const;

    virtual bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* rd) const;

    virtual void deleteRecord(OperationContext* opCtx, const RecordId& dl);

    virtual StatusWith<RecordId> insertRecord(
        OperationContext* opCtx, const char* data, int len, Timestamp, bool enforceQuota);

    virtual Status insertRecordsWithDocWriter(OperationContext* opCtx,
                                              const DocWriter* const* docs,
                                              const Timestamp*,
                                              size_t nDocs,
                                              RecordId* idsOut);

    virtual Status updateRecord(OperationContext* opCtx,
                                const RecordId& oldLocation,
                                const char* data,
                                int len,
                                bool enforceQuota,
                                UpdateNotifier* notifier);

    virtual bool updateWithDamagesSupported() const;

    virtual StatusWith<RecordData> updateWithDamages(OperationContext* opCtx,
                                                     const RecordId& loc,
                                                     const RecordData& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages);

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const final;

    virtual Status truncate(OperationContext* opCtx);

    virtual void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive);

    virtual Status validate(OperationContext* opCtx,
                            ValidateCmdLevel level,
                            ValidateAdaptor* adaptor,
                            ValidateResults* results,
                            BSONObjBuilder* output);

    virtual void appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* result,
                                   double scale) const;

    virtual Status touch(OperationContext* opCtx, BSONObjBuilder* output) const;

    virtual void increaseStorageSize(OperationContext* opCtx, int size, bool enforceQuota);

    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = NULL,
                                int infoLevel = 0) const;

    virtual long long dataSize(OperationContext* opCtx) const {
        return _data->dataSize;
    }

    virtual long long numRecords(OperationContext* opCtx) const {
        return _data->records.size();
    }

    virtual boost::optional<RecordId> oplogStartHack(OperationContext* opCtx,
                                                     const RecordId& startingPosition) const;

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const override {}

    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize) {
        invariant(_data->records.size() == size_t(numRecords));
        _data->dataSize = dataSize;
    }

protected:
    struct EphemeralForTestRecord {
        EphemeralForTestRecord() : size(0) {}
        EphemeralForTestRecord(int size) : size(size), data(new char[size]) {}

        RecordData toRecordData() const {
            return RecordData(data.get(), size);
        }

        int size;
        boost::shared_array<char> data;
    };

    virtual const EphemeralForTestRecord* recordFor(const RecordId& loc) const;
    virtual EphemeralForTestRecord* recordFor(const RecordId& loc);

public:
    //
    // Not in RecordStore interface
    //

    typedef std::map<RecordId, EphemeralForTestRecord> Records;

    bool isCapped() const {
        return _isCapped;
    }
    void setCappedCallback(CappedCallback* cb) {
        _cappedCallback = cb;
    }

private:
    class InsertChange;
    class RemoveChange;
    class TruncateChange;

    class Cursor;
    class ReverseCursor;

    StatusWith<RecordId> extractAndCheckLocForOplog(const char* data, int len) const;

    RecordId allocateLoc();
    bool cappedAndNeedDelete_inlock(OperationContext* opCtx) const;
    void cappedDeleteAsNeeded_inlock(OperationContext* opCtx);
    void deleteRecord_inlock(OperationContext* opCtx, const RecordId& dl);

    // TODO figure out a proper solution to metadata
    const bool _isCapped;
    const int64_t _cappedMaxSize;
    const int64_t _cappedMaxDocs;
    CappedCallback* _cappedCallback;

    // This is the "persistent" data.
    struct Data {
        Data(StringData ns, bool isOplog)
            : dataSize(0), recordsMutex(), nextId(1), isOplog(isOplog) {}

        int64_t dataSize;
        stdx::recursive_mutex recordsMutex;
        Records records;
        int64_t nextId;
        const bool isOplog;
    };

    Data* const _data;
};

}  // namespace mongo
