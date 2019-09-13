
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/db/repair_database.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/storage/mmap_v1/repair_database_interface.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::endl;
using std::string;

using IndexVersion = IndexDescriptor::IndexVersion;

StatusWith<IndexNameObjs> getIndexNameObjs(OperationContext* opCtx,
                                           DatabaseCatalogEntry* dbce,
                                           CollectionCatalogEntry* cce,
                                           stdx::function<bool(const std::string&)> filter) {
    IndexNameObjs ret;
    std::vector<string>& indexNames = ret.first;
    std::vector<BSONObj>& indexSpecs = ret.second;
    {
        // Fetch all indexes
        cce->getAllIndexes(opCtx, &indexNames);
        auto newEnd =
            std::remove_if(indexNames.begin(),
                           indexNames.end(),
                           [&filter](const std::string& indexName) { return !filter(indexName); });
        indexNames.erase(newEnd, indexNames.end());

        indexSpecs.reserve(indexNames.size());

        for (size_t i = 0; i < indexNames.size(); i++) {
            const string& name = indexNames[i];
            BSONObj spec = cce->getIndexSpec(opCtx, name);

            IndexVersion newIndexVersion = IndexVersion::kV0;
            {
                BSONObjBuilder bob;

                for (auto&& indexSpecElem : spec) {
                    auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
                    if (IndexDescriptor::kIndexVersionFieldName == indexSpecElemFieldName) {
                        IndexVersion indexVersion =
                            static_cast<IndexVersion>(indexSpecElem.numberInt());
                        if (IndexVersion::kV0 == indexVersion) {
                            // We automatically upgrade v=0 indexes to v=1 indexes.
                            newIndexVersion = IndexVersion::kV1;
                        } else {
                            newIndexVersion = indexVersion;
                        }

                        bob.append(IndexDescriptor::kIndexVersionFieldName,
                                   static_cast<int>(newIndexVersion));
                    } else {
                        bob.append(indexSpecElem);
                    }
                }

                indexSpecs.push_back(bob.obj());
            }

            const BSONObj key = spec.getObjectField("key");
            const Status keyStatus = index_key_validate::validateKeyPattern(key, newIndexVersion);
            if (!keyStatus.isOK()) {
                return Status(
                    ErrorCodes::CannotCreateIndex,
                    str::stream()
                        << "Cannot rebuild index "
                        << spec
                        << ": "
                        << keyStatus.reason()
                        << " For more info see http://dochub.mongodb.org/core/index-validation");
            }
        }
    }

    return ret;
}

Status rebuildIndexesOnCollection(OperationContext* opCtx,
                                  DatabaseCatalogEntry* dbce,
                                  CollectionCatalogEntry* cce,
                                  const IndexNameObjs& indexNameObjs) {
    const std::vector<std::string>& indexNames = indexNameObjs.first;
    const std::vector<BSONObj>& indexSpecs = indexNameObjs.second;

    // Skip the rest if there are no indexes to rebuild.
    if (indexSpecs.empty())
        return Status::OK();

    std::unique_ptr<Collection> collection;
    std::unique_ptr<MultiIndexBlock> indexer;
    {
        // These steps are combined into a single WUOW to ensure there are no commits without
        // the indexes.
        // 1) Drop all indexes.
        // 2) Open the Collection
        // 3) Start the index build process.

        WriteUnitOfWork wuow(opCtx);

        {  // 1
            for (size_t i = 0; i < indexNames.size(); i++) {
                Status s = cce->removeIndex(opCtx, indexNames[i]);
                if (!s.isOK())
                    return s;
            }
        }

        // Indexes must be dropped before we open the Collection otherwise we could attempt to
        // open a bad index and fail.
        // TODO see if MultiIndexBlock can be made to work without a Collection.
        const StringData ns = cce->ns().ns();
        const auto uuid = cce->getCollectionOptions(opCtx).uuid;
        collection.reset(new Collection(opCtx, ns, uuid, cce, dbce->getRecordStore(ns), dbce));

        indexer.reset(new MultiIndexBlock(opCtx, collection.get()));
        Status status = indexer->init(indexSpecs).getStatus();
        if (!status.isOK()) {
            // The WUOW will handle cleanup, so the indexer shouldn't do its own.
            indexer->abortWithoutCleanup();
            return status;
        }

        wuow.commit();
    }

    // Iterate all records in the collection. Delete them if they aren't valid BSON. Index them
    // if they are.

    long long numRecords = 0;
    long long dataSize = 0;

    RecordStore* rs = collection->getRecordStore();
    auto cursor = rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        RecordId id = record->id;
        RecordData& data = record->data;

        // Use the latest BSON validation version. We retain decimal data when repairing the
        // database even if decimal is disabled.
        Status status = validateBSON(data.data(), data.size(), BSONVersion::kLatest);
        if (!status.isOK()) {
            log() << "Invalid BSON detected at " << id << ": " << redact(status) << ". Deleting.";
            cursor->save();  // 'data' is no longer valid.
            {
                WriteUnitOfWork wunit(opCtx);
                rs->deleteRecord(opCtx, id);
                wunit.commit();
            }
            cursor->restore();
            continue;
        }

        numRecords++;
        dataSize += data.size();

        // Now index the record.
        // TODO SERVER-14812 add a mode that drops duplicates rather than failing
        WriteUnitOfWork wunit(opCtx);
        status = indexer->insert(data.releaseToBson(), id);
        if (!status.isOK())
            return status;
        wunit.commit();
    }

    Status status = indexer->doneInserting();
    if (!status.isOK())
        return status;

    {
        WriteUnitOfWork wunit(opCtx);
        indexer->commit();
        rs->updateStatsAfterRepair(opCtx, numRecords, dataSize);
        wunit.commit();
    }

    return Status::OK();
}

namespace {
Status repairCollections(OperationContext* opCtx,
                         StorageEngine* engine,
                         const std::string& dbName) {

    DatabaseCatalogEntry* dbce = engine->getDatabaseCatalogEntry(opCtx, dbName);

    std::list<std::string> colls;
    dbce->getCollectionNamespaces(&colls);

    for (std::list<std::string>::const_iterator it = colls.begin(); it != colls.end(); ++it) {
        // Don't check for interrupt after starting to repair a collection otherwise we can
        // leave data in an inconsistent state. Interrupting between collections is ok, however.
        opCtx->checkForInterrupt();

        log() << "Repairing collection " << *it;

        Status status = engine->repairRecordStore(opCtx, *it);
        if (!status.isOK())
            return status;

        CollectionCatalogEntry* cce = dbce->getCollectionCatalogEntry(*it);
        auto swIndexNameObjs = getIndexNameObjs(opCtx, dbce, cce);
        if (!swIndexNameObjs.isOK())
            return swIndexNameObjs.getStatus();

        status = rebuildIndexesOnCollection(opCtx, dbce, cce, swIndexNameObjs.getValue());
        if (!status.isOK())
            return status;
    }
    return Status::OK();
}
}  // namespace

Status repairDatabase(OperationContext* opCtx,
                      StorageEngine* engine,
                      const std::string& dbName,
                      bool preserveClonedFilesOnFailure,
                      bool backupOriginalFiles) {
    DisableDocumentValidation validationDisabler(opCtx);

    // We must hold some form of lock here
    invariant(opCtx->lockState()->isLocked());
    invariant(dbName.find('.') == string::npos);

    log() << "repairDatabase " << dbName << endl;

    BackgroundOperation::assertNoBgOpInProgForDb(dbName);

    opCtx->checkForInterrupt();

    if (engine->isMmapV1()) {
        // MMAPv1 is a layering violation so it implements its own repairDatabase. Call through a
        // shimmed interface, so the symbol can exist independent of mmapv1.
        auto status = repairDatabaseMmapv1(
            engine, opCtx, dbName, preserveClonedFilesOnFailure, backupOriginalFiles);
        // Restore oplog Collection pointer cache.
        repl::acquireOplogCollectionForLogging(opCtx);
        return status;
    }

    // These are MMAPv1 specific
    if (preserveClonedFilesOnFailure) {
        return Status(ErrorCodes::BadValue, "preserveClonedFilesOnFailure not supported");
    }
    if (backupOriginalFiles) {
        return Status(ErrorCodes::BadValue, "backupOriginalFiles not supported");
    }

    // Close the db and invalidate all current users and caches.
    DatabaseHolder::getDatabaseHolder().close(opCtx, dbName, "database closed for repair");
    ON_BLOCK_EXIT([&dbName, &opCtx] {
        try {
            // Ensure that we don't trigger an exception when attempting to take locks.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());

            // Open the db after everything finishes.
            auto db = DatabaseHolder::getDatabaseHolder().openDb(opCtx, dbName);

            // Set the minimum snapshot for all Collections in this db. This ensures that readers
            // using majority readConcern level can only use the collections after their repaired
            // versions are in the committed view.
            auto clusterTime = LogicalClock::getClusterTimeForReplicaSet(opCtx).asTimestamp();

            for (auto&& collection : *db) {
                collection->setMinimumVisibleSnapshot(clusterTime);
            }

            // Restore oplog Collection pointer cache.
            repl::acquireOplogCollectionForLogging(opCtx);
        } catch (...) {
            severe() << "Unexpected exception encountered while reopening database after repair.";
            std::terminate();  // Logs additional info about the specific error.
        }
    });

    auto status = repairCollections(opCtx, engine, dbName);
    if (!status.isOK()) {
        severe() << "Failed to repair database " << dbName << ": " << status.reason();
        return status;
    }

    return Status::OK();
}
}
