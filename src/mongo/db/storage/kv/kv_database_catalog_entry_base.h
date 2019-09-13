
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

#include "mongo/db/catalog/database_catalog_entry.h"

namespace mongo {

class KVStorageEngine;
class KVCollectionCatalogEntry;

class KVDatabaseCatalogEntryBase : public DatabaseCatalogEntry {
public:
    KVDatabaseCatalogEntryBase(StringData db, KVStorageEngine* engine);
    ~KVDatabaseCatalogEntryBase() override;

    bool exists() const override;
    bool isEmpty() const override;
    bool hasUserData() const override;

    int64_t sizeOnDisk(OperationContext* opCtx) const override;

    void appendExtraStats(OperationContext* opCtx,
                          BSONObjBuilder* out,
                          double scale) const override;

    bool isOlderThan24(OperationContext* opCtx) const override {
        return false;
    }
    void markIndexSafe24AndUp(OperationContext* opCtx) override {}

    Status currentFilesCompatible(OperationContext* opCtx) const override;

    void getCollectionNamespaces(std::list<std::string>* out) const override;

    CollectionCatalogEntry* getCollectionCatalogEntry(StringData ns) const override;

    RecordStore* getRecordStore(StringData ns) const override;

    IndexAccessMethod* getIndex(OperationContext* opCtx,
                                const CollectionCatalogEntry* collection,
                                IndexCatalogEntry* index) override = 0;

    Status createCollection(OperationContext* opCtx,
                            StringData ns,
                            const CollectionOptions& options,
                            bool allocateDefaultSpace) override;

    Status renameCollection(OperationContext* opCtx,
                            StringData fromNS,
                            StringData toNS,
                            bool stayTemp) override;

    Status dropCollection(OperationContext* opCtx, StringData ns) override;

    // --------------

    void initCollection(OperationContext* opCtx, const std::string& ns, bool forRepair);

    void initCollectionBeforeRepair(OperationContext* opCtx, const std::string& ns);
    void reinitCollectionAfterRepair(OperationContext* opCtx, const std::string& ns);

protected:
    class AddCollectionChange;
    class RemoveCollectionChange;

    typedef std::map<std::string, KVCollectionCatalogEntry*> CollectionMap;


    KVStorageEngine* const _engine;  // not owned here
    CollectionMap _collections;
};
}  // namespace mongo
