// namespace_details_rsv1_metadata.h


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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_base.h"

namespace mongo {

class RecordStore;

/*
 * NOTE: NamespaceDetails will become a struct
 *      all dur, etc... will move here
 */
class NamespaceDetailsRSV1MetaData : public RecordStoreV1MetaData {
public:
    explicit NamespaceDetailsRSV1MetaData(StringData ns, NamespaceDetails* details);

    virtual ~NamespaceDetailsRSV1MetaData() {}

    virtual const DiskLoc& capExtent() const;
    virtual void setCapExtent(OperationContext* opCtx, const DiskLoc& loc);

    virtual const DiskLoc& capFirstNewRecord() const;
    virtual void setCapFirstNewRecord(OperationContext* opCtx, const DiskLoc& loc);

    virtual bool capLooped() const;

    virtual long long dataSize() const;
    virtual long long numRecords() const;

    virtual void incrementStats(OperationContext* opCtx,
                                long long dataSizeIncrement,
                                long long numRecordsIncrement);

    virtual void setStats(OperationContext* opCtx, long long dataSize, long long numRecords);

    virtual DiskLoc deletedListEntry(int bucket) const;
    virtual void setDeletedListEntry(OperationContext* opCtx, int bucket, const DiskLoc& loc);

    virtual DiskLoc deletedListLegacyGrabBag() const;
    virtual void setDeletedListLegacyGrabBag(OperationContext* opCtx, const DiskLoc& loc);

    virtual void orphanDeletedList(OperationContext* opCtx);

    virtual const DiskLoc& firstExtent(OperationContext* opCtx) const;
    virtual void setFirstExtent(OperationContext* opCtx, const DiskLoc& loc);

    virtual const DiskLoc& lastExtent(OperationContext* opCtx) const;
    virtual void setLastExtent(OperationContext* opCtx, const DiskLoc& loc);

    virtual bool isCapped() const;

    virtual bool isUserFlagSet(int flag) const;
    virtual int userFlags() const;
    virtual bool setUserFlag(OperationContext* opCtx, int flag);
    virtual bool clearUserFlag(OperationContext* opCtx, int flag);
    virtual bool replaceUserFlags(OperationContext* opCtx, int flags);

    virtual int lastExtentSize(OperationContext* opCtx) const;
    virtual void setLastExtentSize(OperationContext* opCtx, int newMax);

    virtual long long maxCappedDocs() const;

private:
    std::string _ns;
    NamespaceDetails* _details;
    RecordStore* _namespaceRecordStore;
};
}
