// record_store_v1_simple.h


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

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/storage/mmap_v1/diskloc.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_base.h"

namespace mongo {

class SimpleRecordStoreV1Cursor;

// used by index and original collections
class SimpleRecordStoreV1 : public RecordStoreV1Base {
public:
    SimpleRecordStoreV1(OperationContext* opCtx,
                        StringData ns,
                        RecordStoreV1MetaData* details,
                        ExtentManager* em,
                        bool isSystemIndexes);

    virtual ~SimpleRecordStoreV1();

    const char* name() const {
        return "SimpleRecordStoreV1";
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const final;

    std::vector<std::unique_ptr<RecordCursor>> getManyCursors(OperationContext* opCtx) const final;

    virtual Status truncate(OperationContext* opCtx);

    virtual void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) {
        invariant(!"cappedTruncateAfter not supported");
    }

    virtual bool compactSupported() const {
        return true;
    }
    virtual bool compactsInPlace() const {
        return false;
    }
    virtual Status compact(OperationContext* opCtx,
                           RecordStoreCompactAdaptor* adaptor,
                           const CompactOptions* options,
                           CompactStats* stats);

protected:
    virtual bool isCapped() const {
        return false;
    }
    virtual bool shouldPadInserts() const {
        return !_details->isUserFlagSet(CollectionOptions::Flag_NoPadding);
    }

    virtual StatusWith<DiskLoc> allocRecord(OperationContext* opCtx,
                                            int lengthWithHeaders,
                                            bool enforceQuota);

    virtual void addDeletedRec(OperationContext* opCtx, const DiskLoc& dloc);

private:
    DiskLoc _allocFromExistingExtents(OperationContext* opCtx, int lengthWithHeaders);

    void _compactExtent(OperationContext* opCtx,
                        const DiskLoc diskloc,
                        int extentNumber,
                        RecordStoreCompactAdaptor* adaptor,
                        const CompactOptions* compactOptions,
                        CompactStats* stats);

    bool _normalCollection;

    friend class SimpleRecordStoreV1Iterator;
};
}
