// namespace_details_collection_entry.h

#pragma once

/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/storage/mmap_v1/diskloc.h"

namespace mongo {

class NamespaceDetails;

class MMAPV1DatabaseCatalogEntry;
;
class RecordStore;
class OperationContext;

class NamespaceDetailsCollectionCatalogEntry : public CollectionCatalogEntry {
public:
    NamespaceDetailsCollectionCatalogEntry(StringData ns,
                                           NamespaceDetails* details,
                                           RecordStore* namespacesRecordStore,
                                           RecordId namespacesRecordId,
                                           RecordStore* indexRecordStore,
                                           MMAPV1DatabaseCatalogEntry* db);

    ~NamespaceDetailsCollectionCatalogEntry() {}

    CollectionOptions getCollectionOptions(OperationContext* txn) const final;

    int getTotalIndexCount(OperationContext* txn) const final;

    int getCompletedIndexCount(OperationContext* txn) const final;

    int getMaxAllowedIndexes() const final;

    void getAllIndexes(OperationContext* txn, std::vector<std::string>* names) const final;

    BSONObj getIndexSpec(OperationContext* txn, StringData idxName) const final;

    bool isIndexMultikey(OperationContext* txn, StringData indexName) const final;
    bool isIndexMultikey(int idxNo) const;

    bool setIndexIsMultikey(OperationContext* txn, int idxNo, bool multikey = true);
    bool setIndexIsMultikey(OperationContext* txn,
                            StringData indexName,
                            bool multikey = true) final;

    RecordId getIndexHead(OperationContext* txn, StringData indexName) const final;

    void setIndexHead(OperationContext* txn, StringData indexName, const RecordId& newHead) final;

    bool isIndexReady(OperationContext* txn, StringData indexName) const final;

    Status removeIndex(OperationContext* txn, StringData indexName) final;

    Status prepareForIndexBuild(OperationContext* txn, const IndexDescriptor* spec) final;

    void indexBuildSuccess(OperationContext* txn, StringData indexName) final;

    void updateTTLSetting(OperationContext* txn,
                          StringData idxName,
                          long long newExpireSeconds) final;

    void updateFlags(OperationContext* txn, int newValue) final;

    void updateValidator(OperationContext* txn,
                         const BSONObj& validator,
                         StringData validationLevel,
                         StringData validationAction) final;

    // not part of interface, but available to my storage engine

    int _findIndexNumber(OperationContext* txn, StringData indexName) const;

    RecordId getNamespacesRecordId() {
        return _namespacesRecordId;
    }

    /**
     * 'txn' is only allowed to be null when called from the constructor.
     */
    void setNamespacesRecordId(OperationContext* txn, RecordId newId);

private:
    NamespaceDetails* _details;
    RecordStore* _namespacesRecordStore;

    // Where this entry lives in the _namespacesRecordStore.
    RecordId _namespacesRecordId;

    RecordStore* _indexRecordStore;
    MMAPV1DatabaseCatalogEntry* _db;

    /**
     * Updates the entry for this namespace in '_namespacesRecordStore', updating
     * '_namespacesRecordId' if necessary.
     */
    void _updateSystemNamespaces(OperationContext* txn, const BSONObj& update);

    friend class MMAPV1DatabaseCatalogEntry;
};
}
