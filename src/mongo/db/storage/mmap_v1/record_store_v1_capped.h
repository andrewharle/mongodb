// record_store_v1_capped.h


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

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/mmap_v1/diskloc.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_base.h"

namespace mongo {

class CappedRecordStoreV1 final : public RecordStoreV1Base {
public:
    CappedRecordStoreV1(OperationContext* opCtx,
                        CappedCallback* collection,
                        StringData ns,
                        RecordStoreV1MetaData* details,
                        ExtentManager* em,
                        bool isSystemIndexes);

    ~CappedRecordStoreV1() final;

    const char* name() const final {
        return "CappedRecordStoreV1";
    }

    Status truncate(OperationContext* opCtx) final;

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     */
    void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) final;

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const final;

    std::vector<std::unique_ptr<RecordCursor>> getManyCursors(OperationContext* opCtx) const final;

    // Start from firstExtent by default.
    DiskLoc firstRecord(OperationContext* opCtx, const DiskLoc& startExtent = DiskLoc()) const;
    // Start from lastExtent by default.
    DiskLoc lastRecord(OperationContext* opCtx, const DiskLoc& startExtent = DiskLoc()) const;

protected:
    bool isCapped() const final {
        return true;
    }
    bool shouldPadInserts() const final {
        return false;
    }

    void setCappedCallback(CappedCallback* cb) final {
        _cappedCallback = cb;
    }

    StatusWith<DiskLoc> allocRecord(OperationContext* opCtx,
                                    int lengthWithHeaders,
                                    bool enforceQuota) final;

    void addDeletedRec(OperationContext* opCtx, const DiskLoc& dloc) final;

private:
    // -- start copy from cap.cpp --
    void _compact(OperationContext* opCtx);
    DiskLoc cappedFirstDeletedInCurExtent() const;
    void setFirstDeletedInCurExtent(OperationContext* opCtx, const DiskLoc& loc);
    void cappedCheckMigrate(OperationContext* opCtx);
    DiskLoc __capAlloc(OperationContext* opCtx, int len);
    bool inCapExtent(const DiskLoc& dl) const;
    DiskLoc cappedListOfAllDeletedRecords() const;
    DiskLoc cappedLastDelRecLastExtent() const;
    void setListOfAllDeletedRecords(OperationContext* opCtx, const DiskLoc& loc);
    void setLastDelRecLastExtent(OperationContext* opCtx, const DiskLoc& loc);
    Extent* theCapExtent() const;
    bool nextIsInCapExtent(const DiskLoc& dl) const;
    void advanceCapExtent(OperationContext* opCtx, StringData ns);
    void cappedTruncateLastDelUpdate(OperationContext* opCtx);

    /**
     * Truncate documents newer than the document at 'end' from the capped
     * collection.  The collection cannot be completely emptied using this
     * function.  An assertion will be thrown if that is attempted.
     * @param inclusive - Truncate 'end' as well iff true
     */
    void cappedTruncateAfter(OperationContext* opCtx, const char* ns, DiskLoc end, bool inclusive);

    void _maybeComplain(OperationContext* opCtx, int len) const;

    // -- end copy from cap.cpp --

    CappedCallback* _cappedCallback;

    OwnedPointerVector<ExtentManager::CacheHint> _extentAdvice;

    friend class CappedRecordStoreV1Iterator;
};
}
