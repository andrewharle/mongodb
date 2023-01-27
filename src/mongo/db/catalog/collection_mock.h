
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

#include "mongo/db/catalog/collection.h"

namespace mongo {

/**
 * This class comprises a mock Collection for use by UUIDCatalog unit tests.
 */
class CollectionMock : virtual public Collection::Impl,
                       virtual CappedCallback,
                       virtual UpdateNotifier {
public:
    CollectionMock(const NamespaceString& ns) : _ns(ns) {}
    ~CollectionMock() = default;

    void init(OperationContext* opCtx) {
        std::abort();
    }

private:
    DatabaseCatalogEntry* dbce() const {
        std::abort();
    }

    CollectionCatalogEntry* details() const {
        std::abort();
    }

    Status aboutToDeleteCapped(OperationContext* opCtx, const RecordId& loc, RecordData data) {
        std::abort();
    }

    Status recordStoreGoingToUpdateInPlace(OperationContext* opCtx, const RecordId& loc) {
        std::abort();
    }
    const NamespaceString _ns;

public:
    const NamespaceString& ns() const {
        return _ns;
    }
    bool ok() const {
        std::abort();
    }

    CollectionCatalogEntry* getCatalogEntry() {
        std::abort();
    }
    const CollectionCatalogEntry* getCatalogEntry() const {
        std::abort();
    }

    CollectionInfoCache* infoCache() {
        std::abort();
    }
    const CollectionInfoCache* infoCache() const {
        std::abort();
    }

    void refreshUUID(OperationContext* opCtx) {
        std::abort();
    }

    const IndexCatalog* getIndexCatalog() const {
        std::abort();
    }
    IndexCatalog* getIndexCatalog() {
        std::abort();
    }

    const RecordStore* getRecordStore() const {
        std::abort();
    }
    RecordStore* getRecordStore() {
        std::abort();
    }

    CursorManager* getCursorManager() const {
        std::abort();
    }

    bool requiresIdIndex() const {
        std::abort();
    }

    Snapshotted<BSONObj> docFor(OperationContext* opCtx, const RecordId& loc) const {
        std::abort();
    }

    bool findDoc(OperationContext* opCtx, const RecordId& loc, Snapshotted<BSONObj>* out) const {
        std::abort();
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx, bool forward) const {
        std::abort();
    }

    std::vector<std::unique_ptr<RecordCursor>> getManyCursors(OperationContext* opCtx) const {
        std::abort();
    }

    void deleteDocument(OperationContext* opCtx,
                        StmtId stmtId,
                        const RecordId& loc,
                        OpDebug* opDebug,
                        bool fromMigrate,
                        bool noWarn,
                        Collection::StoreDeletedDoc storeDeletedDoc) {
        std::abort();
    }

    Status insertDocuments(OperationContext* opCtx,
                           std::vector<InsertStatement>::const_iterator begin,
                           std::vector<InsertStatement>::const_iterator end,
                           OpDebug* opDebug,
                           bool enforceQuota,
                           bool fromMigrate) {
        std::abort();
    }

    Status insertDocument(OperationContext* opCtx,
                          const InsertStatement& doc,
                          OpDebug* opDebug,
                          bool enforceQuota,
                          bool fromMigrate) {
        std::abort();
    }

    Status insertDocumentsForOplog(OperationContext* opCtx,
                                   const DocWriter* const* docs,
                                   Timestamp* timestamps,
                                   size_t nDocs) {
        std::abort();
    }

    Status insertDocument(OperationContext* opCtx,
                          const BSONObj& doc,
                          const OnRecordInsertedFn& onRecordInserted,
                          bool enforceQuota) {
        std::abort();
    }

    RecordId updateDocument(OperationContext* opCtx,
                            const RecordId& oldLocation,
                            const Snapshotted<BSONObj>& oldDoc,
                            const BSONObj& newDoc,
                            bool enforceQuota,
                            bool indexesAffected,
                            OpDebug* opDebug,
                            OplogUpdateEntryArgs* args) {
        std::abort();
    }

    bool updateWithDamagesSupported() const {
        std::abort();
    }

    StatusWith<RecordData> updateDocumentWithDamages(OperationContext* opCtx,
                                                     const RecordId& loc,
                                                     const Snapshotted<RecordData>& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages,
                                                     OplogUpdateEntryArgs* args) {
        std::abort();
    }

    StatusWith<CompactStats> compact(OperationContext* opCtx, const CompactOptions* options) {
        std::abort();
    }
    Status truncate(OperationContext* opCtx) {
        std::abort();
    }

    Status validate(OperationContext* opCtx,
                    ValidateCmdLevel level,
                    bool background,
                    std::unique_ptr<Lock::CollectionLock> collLk,
                    ValidateResults* results,
                    BSONObjBuilder* output) {
        std::abort();
    }

    Status touch(OperationContext* opCtx,
                 bool touchData,
                 bool touchIndexes,
                 BSONObjBuilder* output) const {
        std::abort();
    }

    void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) {
        std::abort();
    }

    StatusWithMatchExpression parseValidator(
        OperationContext* opCtx,
        const BSONObj& validator,
        MatchExpressionParser::AllowedFeatureSet allowedFeatures,
        boost::optional<ServerGlobalParams::FeatureCompatibility::Version>
            maxFeatureCompatibilityVersion) const {
        std::abort();
    }

    Status setValidator(OperationContext* opCtx, BSONObj validator) {
        std::abort();
    }

    Status setValidationLevel(OperationContext* opCtx, StringData newLevel) {
        std::abort();
    }
    Status setValidationAction(OperationContext* opCtx, StringData newAction) {
        std::abort();
    }

    StringData getValidationLevel() const {
        std::abort();
    }
    StringData getValidationAction() const {
        std::abort();
    }

    Status updateValidator(OperationContext* opCtx,
                           BSONObj newValidator,
                           StringData newLevel,
                           StringData newAction) {
        std::abort();
    }

    bool isCapped() const {
        std::abort();
    }

    std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier() const {
        std::abort();
    }

    uint64_t numRecords(OperationContext* opCtx) const {
        std::abort();
    }

    uint64_t dataSize(OperationContext* opCtx) const {
        std::abort();
    }

    uint64_t getIndexSize(OperationContext* opCtx, BSONObjBuilder* details, int scale) {
        std::abort();
    }

    boost::optional<Timestamp> getMinimumVisibleSnapshot() {
        std::abort();
    }

    void setMinimumVisibleSnapshot(Timestamp name) {
        std::abort();
    }

    bool haveCappedWaiters() {
        return false;
    }

    void notifyCappedWaitersIfNeeded() {
        std::abort();
    }

    const CollatorInterface* getDefaultCollator() const {
        std::abort();
    }

    OptionalCollectionUUID uuid() const {
        std::abort();
    }
};
}  // namespace mongo
