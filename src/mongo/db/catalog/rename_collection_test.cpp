
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

#include <set>
#include <string>
#include <vector>

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace {

using namespace mongo;

/**
 * Mock OpObserver that tracks dropped collections and databases.
 * Since this class is used exclusively to test dropDatabase(), we will also check the drop-pending
 * flag in the Database object being tested (if provided).
 */
class OpObserverMock : public OpObserverNoop {
public:
    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       OptionalCollectionUUID uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) override;

    void onInserts(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override;

    void onCreateCollection(OperationContext* opCtx,
                            Collection* coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex,
                            const OplogSlot& createOpTime) override;

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid) override;

    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            OptionalCollectionUUID uuid,
                            OptionalCollectionUUID dropTargetUUID,
                            bool stayTemp) override;

    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     OptionalCollectionUUID uuid,
                                     OptionalCollectionUUID dropTargetUUID,
                                     bool stayTemp) override;
    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              OptionalCollectionUUID uuid,
                              OptionalCollectionUUID dropTargetUUID,
                              bool stayTemp) override;
    // Operations written to the oplog. These are operations for which
    // ReplicationCoordinator::isOplogDisabled() returns false.
    std::vector<std::string> oplogEntries;

    bool onInsertsThrows = false;
    bool onInsertsIsGlobalWriteLockExclusive = false;

    bool onRenameCollectionCalled = false;
    OptionalCollectionUUID onRenameCollectionDropTarget;
    repl::OpTime renameOpTime = {Timestamp(Seconds(100), 1U), 1LL};

private:
    /**
     * Pushes 'operationName' into 'oplogEntries' if we can write to the oplog for this namespace.
     */
    void _logOp(OperationContext* opCtx,
                const NamespaceString& nss,
                const std::string& operationName);
};

void OpObserverMock::onCreateIndex(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   OptionalCollectionUUID uuid,
                                   BSONObj indexDoc,
                                   bool fromMigrate) {
    _logOp(opCtx, nss, "index");
    OpObserverNoop::onCreateIndex(opCtx, nss, uuid, indexDoc, fromMigrate);
}

void OpObserverMock::onInserts(OperationContext* opCtx,
                               const NamespaceString& nss,
                               OptionalCollectionUUID uuid,
                               std::vector<InsertStatement>::const_iterator begin,
                               std::vector<InsertStatement>::const_iterator end,
                               bool fromMigrate) {
    if (onInsertsThrows) {
        uasserted(ErrorCodes::OperationFailed, "insert failed");
    }

    // Check global lock state.
    auto lockState = opCtx->lockState();
    ASSERT_TRUE(lockState->isWriteLocked());
    onInsertsIsGlobalWriteLockExclusive = lockState->isW();

    _logOp(opCtx, nss, "inserts");
    OpObserverNoop::onInserts(opCtx, nss, uuid, begin, end, fromMigrate);
}

void OpObserverMock::onCreateCollection(OperationContext* opCtx,
                                        Collection* coll,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex,
                                        const OplogSlot& createOpTime) {
    _logOp(opCtx, collectionName, "create");
    OpObserverNoop::onCreateCollection(opCtx, coll, collectionName, options, idIndex, createOpTime);
}

repl::OpTime OpObserverMock::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              OptionalCollectionUUID uuid) {
    _logOp(opCtx, collectionName, "drop");
    OpObserver::Times::get(opCtx).reservedOpTimes.push_back(
        OpObserverNoop::onDropCollection(opCtx, collectionName, uuid));
    return {};
}

void OpObserverMock::onRenameCollection(OperationContext* opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        OptionalCollectionUUID uuid,
                                        OptionalCollectionUUID dropTargetUUID,
                                        bool stayTemp) {
    preRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
    OpObserverNoop::onRenameCollection(
        opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
    onRenameCollectionCalled = true;
    onRenameCollectionDropTarget = dropTargetUUID;
}

void OpObserverMock::postRenameCollection(OperationContext* opCtx,
                                          const NamespaceString& fromCollection,
                                          const NamespaceString& toCollection,
                                          OptionalCollectionUUID uuid,
                                          OptionalCollectionUUID dropTargetUUID,
                                          bool stayTemp) {
    OpObserverNoop::postRenameCollection(
        opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
    onRenameCollectionCalled = true;
    onRenameCollectionDropTarget = dropTargetUUID;
}

repl::OpTime OpObserverMock::preRenameCollection(OperationContext* opCtx,
                                                 const NamespaceString& fromCollection,
                                                 const NamespaceString& toCollection,
                                                 OptionalCollectionUUID uuid,
                                                 OptionalCollectionUUID dropTargetUUID,
                                                 bool stayTemp) {
    _logOp(opCtx, fromCollection, "rename");
    OpObserver::Times::get(opCtx).reservedOpTimes.push_back(renameOpTime);
    OpObserverNoop::preRenameCollection(
        opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
    return {};
}
void OpObserverMock::_logOp(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const std::string& operationName) {
    if (repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, nss)) {
        return;
    }
    oplogEntries.push_back(operationName);
}

class RenameCollectionTest : public ServiceContextMongoDTest {
public:
    static ServiceContext::UniqueOperationContext makeOpCtx();

private:
    void setUp() override;
    void tearDown() override;

protected:
    ServiceContext::UniqueOperationContext _opCtx;
    repl::ReplicationCoordinatorMock* _replCoord = nullptr;
    OpObserverMock* _opObserver = nullptr;
    NamespaceString _sourceNss;
    NamespaceString _targetNss;
    NamespaceString _targetNssDifferentDb;
};

// static
ServiceContext::UniqueOperationContext RenameCollectionTest::makeOpCtx() {
    return cc().makeOperationContext();
}

void RenameCollectionTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    _opCtx = cc().makeOperationContext();

    repl::StorageInterface::set(service, stdx::make_unique<repl::StorageInterfaceMock>());
    repl::DropPendingCollectionReaper::set(
        service,
        stdx::make_unique<repl::DropPendingCollectionReaper>(repl::StorageInterface::get(service)));

    // Set up ReplicationCoordinator and create oplog.
    auto replCoord = stdx::make_unique<repl::ReplicationCoordinatorMock>(service);
    _replCoord = replCoord.get();
    repl::ReplicationCoordinator::set(service, std::move(replCoord));
    repl::setOplogCollectionName(service);
    repl::createOplog(_opCtx.get());

    // Ensure that we are primary.
    ASSERT_OK(_replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

    // Use OpObserverMock to track notifications for collection and database drops.
    auto opObserver = stdx::make_unique<OpObserverRegistry>();
    auto mockObserver = stdx::make_unique<OpObserverMock>();
    _opObserver = mockObserver.get();
    opObserver->addObserver(std::move(mockObserver));
    opObserver->addObserver(stdx::make_unique<UUIDCatalogObserver>());
    service->setOpObserver(std::move(opObserver));

    _sourceNss = NamespaceString("test.foo");
    _targetNss = NamespaceString("test.bar");
    _targetNssDifferentDb = NamespaceString("test2.bar");
}

void RenameCollectionTest::tearDown() {
    _targetNss = {};
    _sourceNss = {};
    _opObserver = nullptr;
    _replCoord = nullptr;
    _opCtx = {};

    auto service = getServiceContext();
    repl::DropPendingCollectionReaper::set(service, {});
    repl::StorageInterface::set(service, {});

    ServiceContextMongoDTest::tearDown();
}

/**
 * Creates a collection without any namespace restrictions.
 */
void _createCollection(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const CollectionOptions options = {}) {
    writeConflictRetry(opCtx, "_createCollection", nss.ns(), [=] {
        AutoGetOrCreateDb autoDb(opCtx, nss.db(), MODE_X);
        auto db = autoDb.getDb();
        ASSERT_TRUE(db) << "Cannot create collection " << nss << " because database " << nss.db()
                        << " does not exist.";

        WriteUnitOfWork wuow(opCtx);
        ASSERT_TRUE(db->createCollection(opCtx, nss.ns(), options))
            << "Failed to create collection " << nss << " due to unknown error.";
        wuow.commit();
    });

    ASSERT_TRUE(AutoGetCollectionForRead(opCtx, nss).getCollection());
}

/**
 * Returns a collection options with a generated UUID.
 */
CollectionOptions _makeCollectionOptionsWithUuid() {
    CollectionOptions options;
    options.uuid = UUID::gen();
    return options;
}

/**
 * Creates a collection with UUID and returns the UUID.
 */
CollectionUUID _createCollectionWithUUID(OperationContext* opCtx, const NamespaceString& nss) {
    const auto options = _makeCollectionOptionsWithUuid();
    _createCollection(opCtx, nss, options);
    return options.uuid.get();
}

/**
 * Returns true if collection exists.
 */
bool _collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
    return AutoGetCollectionForRead(opCtx, nss).getCollection() != nullptr;
}

/**
 * Returns collection options.
 */
CollectionOptions _getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForRead autoColl(opCtx, nss);
    auto collection = autoColl.getCollection();
    ASSERT_TRUE(collection) << "Unable to get collections options for " << nss
                            << " because collection does not exist.";
    auto catalogEntry = collection->getCatalogEntry();
    return catalogEntry->getCollectionOptions(opCtx);
}

/**
 * Returns UUID of collection.
 */
CollectionUUID _getCollectionUuid(OperationContext* opCtx, const NamespaceString& nss) {
    auto options = _getCollectionOptions(opCtx, nss);
    ASSERT_TRUE(options.uuid);
    return *(options.uuid);
}

/**
 * Get collection namespace by UUID.
 */
NamespaceString _getCollectionNssFromUUID(OperationContext* opCtx, const UUID& uuid) {
    Collection* source = UUIDCatalog::get(opCtx).lookupCollectionByUUID(uuid);
    return source ? source->ns() : NamespaceString();
}

/**
 * Returns true if namespace refers to a temporary collection.
 */
bool _isTempCollection(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForRead autoColl(opCtx, nss);
    auto collection = autoColl.getCollection();
    ASSERT_TRUE(collection) << "Unable to check if " << nss
                            << " is a temporary collection because collection does not exist.";
    auto catalogEntry = collection->getCatalogEntry();
    auto options = catalogEntry->getCollectionOptions(opCtx);
    return options.temp;
}

/**
 * Creates an index using the given index name with a bogus key spec.
 */
void _createIndex(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const std::string& indexName) {
    writeConflictRetry(opCtx, "_createIndex", nss.ns(), [=] {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        auto collection = autoColl.getCollection();
        ASSERT_TRUE(collection) << "Cannot create index in collection " << nss
                                << " because collection " << nss.ns() << " does not exist.";

        auto indexInfoObj = BSON(
            "v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << BSON("a" << 1) << "name"
                << indexName
                << "ns"
                << nss.ns());

        MultiIndexBlock indexer(opCtx, collection);
        ASSERT_OK(indexer.init(indexInfoObj).getStatus());
        WriteUnitOfWork wuow(opCtx);
        indexer.commit();
        wuow.commit();
    });

    ASSERT_TRUE(AutoGetCollectionForRead(opCtx, nss).getCollection());
}

/**
 * Inserts a single document into a collection.
 */
void _insertDocument(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc) {
    writeConflictRetry(opCtx, "_insertDocument", nss.ns(), [=] {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        auto collection = autoColl.getCollection();
        ASSERT_TRUE(collection) << "Cannot insert document " << doc << " into collection " << nss
                                << " because collection " << nss.ns() << " does not exist.";

        WriteUnitOfWork wuow(opCtx);
        OpDebug* const opDebug = nullptr;
        bool enforceQuota = true;
        ASSERT_OK(collection->insertDocument(opCtx, InsertStatement(doc), opDebug, enforceQuota));
        wuow.commit();
    });
}

TEST_F(RenameCollectionTest, RenameCollectionReturnsNamespaceNotFoundIfDatabaseDoesNotExist) {
    ASSERT_FALSE(AutoGetDb(_opCtx.get(), _sourceNss.db(), MODE_X).getDb());
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  renameCollection(_opCtx.get(), _sourceNss, _targetNss, {}));
}

TEST_F(RenameCollectionTest,
       RenameCollectionReturnsNamespaceNotFoundIfSourceCollectionIsDropPending) {
    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    auto dropPendingNss = _sourceNss.makeDropPendingNamespace(dropOpTime);

    _createCollection(_opCtx.get(), dropPendingNss);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  renameCollection(_opCtx.get(), dropPendingNss, _targetNss, {}));

    // Source collections stays in drop-pending state.
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _targetNss));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), dropPendingNss));
}

TEST_F(RenameCollectionTest, RenameCollectionReturnsNotMasterIfNotPrimary) {
    _createCollection(_opCtx.get(), _sourceNss);
    ASSERT_OK(_replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(_replCoord->canAcceptWritesForDatabase(_opCtx.get(), _sourceNss.db()));
    ASSERT_EQUALS(ErrorCodes::NotMaster,
                  renameCollection(_opCtx.get(), _sourceNss, _targetNss, {}));
}

TEST_F(RenameCollectionTest, IndexNameTooLongForTargetCollection) {
    ASSERT_GREATER_THAN(_targetNssDifferentDb.size(), _sourceNss.size());
    std::size_t longestIndexNameAllowedForSource =
        NamespaceString::MaxNsLen - 2U /*strlen(".$")*/ - _sourceNss.size();
    ASSERT_OK(_sourceNss.checkLengthForRename(longestIndexNameAllowedForSource));
    ASSERT_EQUALS(ErrorCodes::InvalidLength,
                  _targetNssDifferentDb.checkLengthForRename(longestIndexNameAllowedForSource));

    _createCollection(_opCtx.get(), _sourceNss);
    const std::string indexName(longestIndexNameAllowedForSource, 'a');
    _createIndex(_opCtx.get(), _sourceNss, indexName);
    ASSERT_EQUALS(ErrorCodes::InvalidLength,
                  renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}));
}

TEST_F(RenameCollectionTest, IndexNameTooLongForTemporaryCollectionForRenameAcrossDatabase) {
    ASSERT_GREATER_THAN(_targetNssDifferentDb.size(), _sourceNss.size());
    std::size_t longestIndexNameAllowedForTarget =
        NamespaceString::MaxNsLen - 2U /*strlen(".$")*/ - _targetNssDifferentDb.size();
    ASSERT_OK(_sourceNss.checkLengthForRename(longestIndexNameAllowedForTarget));
    ASSERT_OK(_targetNssDifferentDb.checkLengthForRename(longestIndexNameAllowedForTarget));

    // Using XXXXX to check namespace length. Each 'X' will be replaced by a random character in
    // renameCollection().
    const NamespaceString tempNss(_targetNssDifferentDb.getSisterNS("tmpXXXXX.renameCollection"));
    ASSERT_EQUALS(ErrorCodes::InvalidLength,
                  tempNss.checkLengthForRename(longestIndexNameAllowedForTarget));

    _createCollection(_opCtx.get(), _sourceNss);
    const std::string indexName(longestIndexNameAllowedForTarget, 'a');
    _createIndex(_opCtx.get(), _sourceNss, indexName);
    ASSERT_EQUALS(ErrorCodes::InvalidLength,
                  renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}));
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabaseWithUuid) {
    auto options = _makeCollectionOptionsWithUuid();
    _createCollection(_opCtx.get(), _sourceNss, options);
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _sourceNss));
    ASSERT_NOT_EQUALS(options.uuid, _getCollectionUuid(_opCtx.get(), _targetNssDifferentDb));
}

TEST_F(RenameCollectionTest,
       RenameCollectionForApplyOpsReturnsNamespaceNotFoundIfSourceCollectionIsDropPending) {
    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    auto dropPendingNss = _sourceNss.makeDropPendingNamespace(dropOpTime);
    _createCollection(_opCtx.get(), dropPendingNss);

    auto dbName = _sourceNss.db().toString();
    auto cmd = BSON("renameCollection" << dropPendingNss.ns() << "to" << _targetNss.ns());
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  renameCollectionForApplyOps(_opCtx.get(), dbName, {}, cmd, {}));

    // Source collections stays in drop-pending state.
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _targetNss));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), dropPendingNss));
}

TEST_F(
    RenameCollectionTest,
    RenameCollectionForApplyOpsReturnsNamespaceNotFoundIfTargetUuidRefersToDropPendingCollection) {
    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    auto dropPendingNss = _sourceNss.makeDropPendingNamespace(dropOpTime);
    auto options = _makeCollectionOptionsWithUuid();
    _createCollection(_opCtx.get(), dropPendingNss, options);

    auto dbName = _sourceNss.db().toString();
    NamespaceString ignoredSourceNss(dbName, "ignored");
    auto uuidDoc = options.uuid->toBSON();
    auto cmd = BSON("renameCollection" << ignoredSourceNss.ns() << "to" << _targetNss.ns());
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  renameCollectionForApplyOps(_opCtx.get(), dbName, uuidDoc["uuid"], cmd, {}));

    // Source collections stays in drop-pending state.
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _targetNss));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), ignoredSourceNss));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), dropPendingNss));
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsAcrossDatabaseWithTargetUuid) {
    _createCollection(_opCtx.get(), _sourceNss);
    auto dbName = _sourceNss.db().toString();
    auto uuid = UUID::gen();
    auto uuidDoc = BSON("ui" << uuid);
    auto cmd = BSON("renameCollection" << _sourceNss.ns() << "to" << _targetNssDifferentDb.ns()
                                       << "dropTarget"
                                       << true);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), dbName, uuidDoc["ui"], cmd, {}));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _sourceNss));
    ASSERT_EQUALS(uuid, _getCollectionUuid(_opCtx.get(), _targetNssDifferentDb));
}

TEST_F(RenameCollectionTest, RenameCollectionToItselfByNsForApplyOps) {
    auto dbName = _sourceNss.db().toString();
    auto uuid = _createCollectionWithUUID(_opCtx.get(), _sourceNss);
    auto uuidDoc = BSON("ui" << uuid);
    auto cmd = BSON("renameCollection" << _sourceNss.ns() << "to" << _sourceNss.ns() << "dropTarget"
                                       << true);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), dbName, uuidDoc["ui"], cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _sourceNss));
}

TEST_F(RenameCollectionTest, RenameCollectionToItselfByUUIDForApplyOps) {
    auto dbName = _targetNss.db().toString();
    auto uuid = _createCollectionWithUUID(_opCtx.get(), _targetNss);
    auto uuidDoc = BSON("ui" << uuid);
    auto cmd = BSON("renameCollection" << _sourceNss.ns() << "to" << _targetNss.ns() << "dropTarget"
                                       << true);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), dbName, uuidDoc["ui"], cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _targetNss));
}

TEST_F(RenameCollectionTest, RenameCollectionByUUIDRatherThanNsForApplyOps) {
    auto realRenameFromNss = NamespaceString("test.bar2");
    auto dbName = realRenameFromNss.db().toString();
    auto uuid = _createCollectionWithUUID(_opCtx.get(), realRenameFromNss);
    auto uuidDoc = BSON("ui" << uuid);
    auto cmd = BSON("renameCollection" << _sourceNss.ns() << "to" << _targetNss.ns() << "dropTarget"
                                       << true);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), dbName, uuidDoc["ui"], cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _targetNss));
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsDropTargetByUUIDTargetDoesNotExist) {
    const auto& collA = NamespaceString("test.A");
    const auto& collB = NamespaceString("test.B");
    const auto& collC = NamespaceString("test.C");
    auto dbName = collA.db().toString();
    auto collAUUID = _createCollectionWithUUID(_opCtx.get(), collA);
    auto collCUUID = _createCollectionWithUUID(_opCtx.get(), collC);
    auto uuidDoc = BSON("ui" << collAUUID);
    // Rename A to B, drop C, where B is not an existing collection
    auto cmd =
        BSON("renameCollection" << collA.ns() << "to" << collB.ns() << "dropTarget" << collCUUID);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), dbName, uuidDoc["ui"], cmd, {}));
    // A and C should be dropped
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collA));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collC));
    // B (originally A) should exist
    ASSERT_TRUE(_collectionExists(_opCtx.get(), collB));
    // collAUUID should be associated with collB's NamespaceString in the UUIDCatalog.
    auto newCollNS = _getCollectionNssFromUUID(_opCtx.get(), collAUUID);
    ASSERT_TRUE(newCollNS.isValid());
    ASSERT_EQUALS(newCollNS, collB);
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsDropTargetByUUIDTargetExists) {
    const auto& collA = NamespaceString("test.A");
    const auto& collB = NamespaceString("test.B");
    const auto& collC = NamespaceString("test.C");
    auto dbName = collA.db().toString();
    auto collAUUID = _createCollectionWithUUID(_opCtx.get(), collA);
    auto collBUUID = _createCollectionWithUUID(_opCtx.get(), collB);
    auto collCUUID = _createCollectionWithUUID(_opCtx.get(), collC);
    auto uuidDoc = BSON("ui" << collAUUID);
    // Rename A to B, drop C, where B is an existing collection
    // B should be kept but with a temporary name
    auto cmd =
        BSON("renameCollection" << collA.ns() << "to" << collB.ns() << "dropTarget" << collCUUID);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), dbName, uuidDoc["ui"], cmd, {}));
    // A and C should be dropped
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collA));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collC));
    // B (originally A) should exist
    ASSERT_TRUE(_collectionExists(_opCtx.get(), collB));
    // The original B should exist too, but with a temporary name
    const auto& tmpB = UUIDCatalog::get(_opCtx.get()).lookupNSSByUUID(collBUUID);
    ASSERT_FALSE(tmpB.isEmpty());
    ASSERT_TRUE(tmpB.coll().startsWith("tmp"));
    ASSERT_TRUE(tmpB != collB);
}

TEST_F(RenameCollectionTest,
       RenameCollectionForApplyOpsDropTargetByUUIDTargetExistsButTemporarily) {

    const auto& collA = NamespaceString("test.A");
    const auto& collB = NamespaceString("test.B");
    const auto& collC = NamespaceString("test.C");

    CollectionOptions collectionOptions = _makeCollectionOptionsWithUuid();
    collectionOptions.temp = true;
    _createCollection(_opCtx.get(), collB, collectionOptions);
    auto collBUUID = _getCollectionUuid(_opCtx.get(), collB);

    auto dbName = collA.db().toString();
    auto collAUUID = _createCollectionWithUUID(_opCtx.get(), collA);
    auto collCUUID = _createCollectionWithUUID(_opCtx.get(), collC);
    auto uuidDoc = BSON("ui" << collAUUID);
    // Rename A to B, drop C, where B is an existing collection
    // B should be kept but with a temporary name
    auto cmd =
        BSON("renameCollection" << collA.ns() << "to" << collB.ns() << "dropTarget" << collCUUID);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), dbName, uuidDoc["ui"], cmd, {}));
    // A and C should be dropped
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collA));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collC));
    // B (originally A) should exist
    ASSERT_TRUE(_collectionExists(_opCtx.get(), collB));
    // The original B should exist too, but with a temporary name
    const auto& tmpB = UUIDCatalog::get(_opCtx.get()).lookupNSSByUUID(collBUUID);
    ASSERT_FALSE(tmpB.isEmpty());
    ASSERT_TRUE(tmpB != collB);
    ASSERT_TRUE(tmpB.coll().startsWith("tmp"));
    ASSERT_TRUE(_isTempCollection(_opCtx.get(), tmpB));
}

TEST_F(RenameCollectionTest,
       RenameCollectionForApplyOpsDropTargetByUUIDTargetExistsButRealDropTargetDoesNotExist) {
    const auto& collA = NamespaceString("test.A");
    const auto& collB = NamespaceString("test.B");
    auto dbName = collA.db().toString();
    auto collAUUID = _createCollectionWithUUID(_opCtx.get(), collA);
    auto collBUUID = _createCollectionWithUUID(_opCtx.get(), collB);
    auto collCUUID = UUID::gen();
    auto uuidDoc = BSON("ui" << collAUUID);
    // Rename A to B, drop C, where B is an existing collection
    // B should be kept but with a temporary name
    auto cmd =
        BSON("renameCollection" << collA.ns() << "to" << collB.ns() << "dropTarget" << collCUUID);
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), dbName, uuidDoc["ui"], cmd, {}));
    // A and C should be dropped
    ASSERT_FALSE(_collectionExists(_opCtx.get(), collA));
    // B (originally A) should exist
    ASSERT_TRUE(_collectionExists(_opCtx.get(), collB));
    // The original B should exist too, but with a temporary name
    const auto& tmpB = UUIDCatalog::get(_opCtx.get()).lookupNSSByUUID(collBUUID);
    ASSERT_FALSE(tmpB.isEmpty());
    ASSERT_TRUE(tmpB != collB);
    ASSERT_TRUE(tmpB.coll().startsWith("tmp"));
}

TEST_F(RenameCollectionTest,
       RenameCollectionReturnsNamespaceExitsIfTargetExistsAndDropTargetIsFalse) {
    _createCollection(_opCtx.get(), _sourceNss);
    _createCollection(_opCtx.get(), _targetNss);
    RenameCollectionOptions options;
    ASSERT_FALSE(options.dropTarget);
    ASSERT_EQUALS(ErrorCodes::NamespaceExists,
                  renameCollection(_opCtx.get(), _sourceNss, _targetNss, options));
}

TEST_F(RenameCollectionTest, RenameCollectionMakesTargetCollectionDropPendingIfDropTargetIsTrue) {
    _createCollectionWithUUID(_opCtx.get(), _sourceNss);
    auto targetUUID = _createCollectionWithUUID(_opCtx.get(), _targetNss);
    RenameCollectionOptions options;
    options.dropTarget = true;
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, _targetNss, options));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _sourceNss))
        << "source collection " << _sourceNss << " still exists after successful rename";
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _targetNss)) << "target collection " << _targetNss
                                                             << " missing after successful rename";

    ASSERT_TRUE(_opObserver->onRenameCollectionCalled);
    ASSERT_EQUALS(targetUUID, _opObserver->onRenameCollectionDropTarget);

    auto renameOpTime = _opObserver->renameOpTime;
    ASSERT_GREATER_THAN(renameOpTime, repl::OpTime());

    // Confirm that the target collection has been renamed to a drop-pending collection.
    auto dpns = _targetNss.makeDropPendingNamespace(renameOpTime);
    ASSERT_TRUE(_collectionExists(_opCtx.get(), dpns))
        << "target collection " << _targetNss
        << " not renamed to drop-pending collection after successful rename";
}

TEST_F(RenameCollectionTest,
       RenameCollectionOverridesDropTargetIfTargetCollectionIsMissingAndDropTargetIsTrue) {
    _createCollectionWithUUID(_opCtx.get(), _sourceNss);
    RenameCollectionOptions options;
    options.dropTarget = true;
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, _targetNss, options));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _sourceNss))
        << "source collection " << _sourceNss << " still exists after successful rename";
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _targetNss)) << "target collection " << _targetNss
                                                             << " missing after successful rename";

    ASSERT_TRUE(_opObserver->onRenameCollectionCalled);
    ASSERT_FALSE(_opObserver->onRenameCollectionDropTarget);
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsRejectsRenameOpTimeIfWritesAreReplicated) {
    ASSERT_TRUE(_opCtx->writesAreReplicated());

    _createCollection(_opCtx.get(), _sourceNss);
    auto dbName = _sourceNss.db().toString();
    auto cmd = BSON("renameCollection" << _sourceNss.ns() << "to" << _targetNss.ns());
    auto renameOpTime = _opObserver->renameOpTime;
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  renameCollectionForApplyOps(_opCtx.get(), dbName, {}, cmd, renameOpTime));
}

TEST_F(RenameCollectionTest,
       RenameCollectionForApplyOpsMakesTargetCollectionDropPendingIfDropTargetIsTrue) {
    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    ASSERT_FALSE(_opCtx->writesAreReplicated());

    // OpObserver::preRenameCollection() must return a null OpTime when writes are not replicated.
    _opObserver->renameOpTime = {};

    _createCollection(_opCtx.get(), _sourceNss);
    _createCollection(_opCtx.get(), _targetNss);
    auto dbName = _sourceNss.db().toString();
    auto cmd = BSON("renameCollection" << _sourceNss.ns() << "to" << _targetNss.ns() << "dropTarget"
                                       << true);

    repl::OpTime renameOpTime = {Timestamp(Seconds(200), 1U), 1LL};
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), dbName, {}, cmd, renameOpTime));

    // Confirm that the target collection has been renamed to a drop-pending collection.
    auto dpns = _targetNss.makeDropPendingNamespace(renameOpTime);
    ASSERT_TRUE(_collectionExists(_opCtx.get(), dpns))
        << "target collection " << _targetNss
        << " not renamed to drop-pending collection after successful rename for applyOps";
}

DEATH_TEST_F(RenameCollectionTest,
             RenameCollectionForApplyOpsTriggersFatalAssertionIfLogOpReturnsValidOpTime,
             "unexpected renameCollection oplog entry written to the oplog with optime") {
    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    ASSERT_FALSE(_opCtx->writesAreReplicated());

    _createCollection(_opCtx.get(), _sourceNss);
    _createCollection(_opCtx.get(), _targetNss);
    auto dbName = _sourceNss.db().toString();
    auto cmd = BSON("renameCollection" << _sourceNss.ns() << "to" << _targetNss.ns() << "dropTarget"
                                       << true);

    repl::OpTime renameOpTime = {Timestamp(Seconds(200), 1U), 1LL};
    ASSERT_OK(renameCollectionForApplyOps(_opCtx.get(), dbName, {}, cmd, renameOpTime));
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsSourceAndTargetDoNotExist) {
    auto uuidDoc = BSON("ui" << UUID::gen());
    auto cmd = BSON("renameCollection" << _sourceNss.ns() << "to" << _targetNss.ns() << "dropTarget"
                                       << "true");
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  renameCollectionForApplyOps(
                      _opCtx.get(), _sourceNss.db().toString(), uuidDoc["ui"], cmd, {}));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _sourceNss));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _targetNss));
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsDropTargetEvenIfSourceDoesNotExist) {
    _createCollectionWithUUID(_opCtx.get(), _targetNss);
    auto missingSourceNss = NamespaceString("test.bar2");
    auto uuidDoc = BSON("ui" << UUID::gen());
    auto cmd =
        BSON("renameCollection" << missingSourceNss.ns() << "to" << _targetNss.ns() << "dropTarget"
                                << "true");
    ASSERT_OK(renameCollectionForApplyOps(
        _opCtx.get(), missingSourceNss.db().toString(), uuidDoc["ui"], cmd, {}));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _targetNss));
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsDropTargetByUUIDEvenIfSourceDoesNotExist) {
    auto missingSourceNss = NamespaceString("test.bar2");
    auto dropTargetNss = NamespaceString("test.bar3");
    _createCollectionWithUUID(_opCtx.get(), _targetNss);
    auto dropTargetUUID = _createCollectionWithUUID(_opCtx.get(), dropTargetNss);
    auto uuidDoc = BSON("ui" << UUID::gen());
    auto cmd =
        BSON("renameCollection" << missingSourceNss.ns() << "to" << _targetNss.ns() << "dropTarget"
                                << dropTargetUUID);
    ASSERT_OK(renameCollectionForApplyOps(
        _opCtx.get(), missingSourceNss.db().toString(), uuidDoc["ui"], cmd, {}));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _targetNss));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), dropTargetNss));
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsDropTargetEvenIfSourceIsDropPending) {
    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    auto dropPendingNss = _sourceNss.makeDropPendingNamespace(dropOpTime);

    auto dropTargetUUID = _createCollectionWithUUID(_opCtx.get(), _targetNss);
    auto uuidDoc = BSON("ui" << _createCollectionWithUUID(_opCtx.get(), dropPendingNss));
    auto cmd =
        BSON("renameCollection" << dropPendingNss.ns() << "to" << _targetNss.ns() << "dropTarget"
                                << "true");

    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    repl::OpTime renameOpTime = {Timestamp(Seconds(200), 1U), 1LL};
    ASSERT_OK(renameCollectionForApplyOps(
        _opCtx.get(), dropPendingNss.db().toString(), uuidDoc["ui"], cmd, renameOpTime));

    // Source collections stays in drop-pending state.
    ASSERT_TRUE(_collectionExists(_opCtx.get(), dropPendingNss));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), _targetNss));
    ASSERT_EQUALS(_targetNss.makeDropPendingNamespace(renameOpTime),
                  _getCollectionNssFromUUID(_opCtx.get(), dropTargetUUID));
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsDropTargetByUUIDEvenIfSourceIsDropPending) {
    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    auto dropPendingNss = _sourceNss.makeDropPendingNamespace(dropOpTime);
    auto dropTargetNss = NamespaceString("test.bar2");

    _createCollectionWithUUID(_opCtx.get(), _targetNss);

    auto dropTargetUUID = _createCollectionWithUUID(_opCtx.get(), dropTargetNss);
    auto uuidDoc = BSON("ui" << _createCollectionWithUUID(_opCtx.get(), dropPendingNss));
    auto cmd =
        BSON("renameCollection" << dropPendingNss.ns() << "to" << _targetNss.ns() << "dropTarget"
                                << dropTargetUUID);

    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    repl::OpTime renameOpTime = {Timestamp(Seconds(200), 1U), 1LL};
    ASSERT_OK(renameCollectionForApplyOps(
        _opCtx.get(), dropPendingNss.db().toString(), uuidDoc["ui"], cmd, renameOpTime));

    // Source collections stays in drop-pending state.
    ASSERT_TRUE(_collectionExists(_opCtx.get(), dropPendingNss));
    ASSERT_FALSE(_collectionExists(_opCtx.get(), dropTargetNss));
    ASSERT_EQUALS(dropTargetNss.makeDropPendingNamespace(renameOpTime),
                  _getCollectionNssFromUUID(_opCtx.get(), dropTargetUUID));
    ASSERT_TRUE(_collectionExists(_opCtx.get(), _targetNss));
}

void _testRenameCollectionStayTemp(OperationContext* opCtx,
                                   const NamespaceString& sourceNss,
                                   const NamespaceString& targetNss,
                                   bool stayTemp,
                                   bool isSourceCollectionTemporary) {
    CollectionOptions collectionOptions;
    collectionOptions.temp = isSourceCollectionTemporary;
    _createCollection(opCtx, sourceNss, collectionOptions);

    RenameCollectionOptions options;
    options.stayTemp = stayTemp;
    ASSERT_OK(renameCollection(opCtx, sourceNss, targetNss, options));
    ASSERT_FALSE(_collectionExists(opCtx, sourceNss)) << "source collection " << sourceNss
                                                      << " still exists after successful rename";

    if (!isSourceCollectionTemporary) {
        ASSERT_FALSE(_isTempCollection(opCtx, targetNss))
            << "target collection " << targetNss
            << " cannot not be temporary after rename if source collection is not temporary.";
    } else if (stayTemp) {
        ASSERT_TRUE(_isTempCollection(opCtx, targetNss))
            << "target collection " << targetNss
            << " is no longer temporary after rename with stayTemp set to true.";
    } else {
        ASSERT_FALSE(_isTempCollection(opCtx, targetNss))
            << "target collection " << targetNss
            << " still temporary after rename with stayTemp set to false.";
    }
}

TEST_F(RenameCollectionTest, RenameSameDatabaseStayTempFalse) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNss, false, true);
}

TEST_F(RenameCollectionTest, RenameSameDatabaseStayTempTrue) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNss, true, true);
}

TEST_F(RenameCollectionTest, RenameDifferentDatabaseStayTempFalse) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNssDifferentDb, false, true);
}

TEST_F(RenameCollectionTest, RenameDifferentDatabaseStayTempTrue) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNssDifferentDb, true, true);
}

TEST_F(RenameCollectionTest, RenameSameDatabaseStayTempFalseSourceNotTemporary) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNss, false, false);
}

TEST_F(RenameCollectionTest, RenameSameDatabaseStayTempTrueSourceNotTemporary) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNss, true, false);
}

TEST_F(RenameCollectionTest, RenameDifferentDatabaseStayTempFalseSourceNotTemporary) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNssDifferentDb, false, false);
}

TEST_F(RenameCollectionTest, RenameDifferentDatabaseStayTempTrueSourceNotTemporary) {
    _testRenameCollectionStayTemp(_opCtx.get(), _sourceNss, _targetNssDifferentDb, true, false);
}

/**
 * Checks oplog entries written by the OpObserver to the oplog.
 */
void _checkOplogEntries(const std::vector<std::string>& actualOplogEntries,
                        const std::vector<std::string>& expectedOplogEntries) {
    std::string actualOplogEntriesStr;
    joinStringDelim(actualOplogEntries, &actualOplogEntriesStr, ',');
    std::string expectedOplogEntriesStr;
    joinStringDelim(expectedOplogEntries, &expectedOplogEntriesStr, ',');
    ASSERT_EQUALS(expectedOplogEntries.size(), actualOplogEntries.size())

        << "Incorrect number of oplog entries written to oplog. Actual: " << actualOplogEntriesStr
        << ". Expected: " << expectedOplogEntriesStr;
    std::vector<std::string>::size_type i = 0;
    for (const auto& actualOplogEntry : actualOplogEntries) {
        const auto& expectedOplogEntry = expectedOplogEntries[i++];
        ASSERT_EQUALS(expectedOplogEntry, actualOplogEntry)
            << "Mismatch in oplog entry at index " << i << ". Actual: " << actualOplogEntriesStr
            << ". Expected: " << expectedOplogEntriesStr;
    }
}

/**
 * Runs a rename across database operation and checks oplog entries writtent to the oplog.
 */
void _testRenameCollectionAcrossDatabaseOplogEntries(
    OperationContext* opCtx,
    const NamespaceString& sourceNss,
    const NamespaceString& targetNss,
    std::vector<std::string>* oplogEntries,
    bool forApplyOps,
    const std::vector<std::string>& expectedOplogEntries) {
    ASSERT_NOT_EQUALS(sourceNss.db(), targetNss.db());
    _createCollection(opCtx, sourceNss);
    _createIndex(opCtx, sourceNss, "a_1");
    _insertDocument(opCtx, sourceNss, BSON("_id" << 0));
    oplogEntries->clear();
    if (forApplyOps) {
        auto cmd = BSON(
            "renameCollection" << sourceNss.ns() << "to" << targetNss.ns() << "dropTarget" << true);
        ASSERT_OK(renameCollectionForApplyOps(opCtx, sourceNss.db().toString(), {}, cmd, {}));
    } else {
        RenameCollectionOptions options;
        options.dropTarget = true;
        ASSERT_OK(renameCollection(opCtx, sourceNss, targetNss, options));
    }
    _checkOplogEntries(*oplogEntries, expectedOplogEntries);
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabaseOplogEntries) {
    bool forApplyOps = false;
    _testRenameCollectionAcrossDatabaseOplogEntries(
        _opCtx.get(),
        _sourceNss,
        _targetNssDifferentDb,
        &_opObserver->oplogEntries,
        forApplyOps,
        {"create", "index", "inserts", "rename", "drop"});
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsAcrossDatabaseOplogEntries) {
    bool forApplyOps = true;
    _testRenameCollectionAcrossDatabaseOplogEntries(
        _opCtx.get(),
        _sourceNss,
        _targetNssDifferentDb,
        &_opObserver->oplogEntries,
        forApplyOps,
        {"create", "index", "inserts", "rename", "drop"});
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabaseOplogEntriesDropTarget) {
    _createCollection(_opCtx.get(), _targetNssDifferentDb);
    bool forApplyOps = false;
    _testRenameCollectionAcrossDatabaseOplogEntries(
        _opCtx.get(),
        _sourceNss,
        _targetNssDifferentDb,
        &_opObserver->oplogEntries,
        forApplyOps,
        {"create", "index", "inserts", "rename", "drop"});
}

TEST_F(RenameCollectionTest, RenameCollectionForApplyOpsAcrossDatabaseOplogEntriesDropTarget) {
    _createCollection(_opCtx.get(), _targetNssDifferentDb);
    bool forApplyOps = true;
    _testRenameCollectionAcrossDatabaseOplogEntries(
        _opCtx.get(),
        _sourceNss,
        _targetNssDifferentDb,
        &_opObserver->oplogEntries,
        forApplyOps,
        {"create", "index", "inserts", "rename", "drop"});
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabaseOplogEntriesWritesNotReplicated) {
    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    bool forApplyOps = false;
    _testRenameCollectionAcrossDatabaseOplogEntries(_opCtx.get(),
                                                    _sourceNss,
                                                    _targetNssDifferentDb,
                                                    &_opObserver->oplogEntries,
                                                    forApplyOps,
                                                    {});
}

TEST_F(RenameCollectionTest,
       RenameCollectionForApplyOpsAcrossDatabaseOplogEntriesWritesNotReplicated) {
    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    bool forApplyOps = true;
    _testRenameCollectionAcrossDatabaseOplogEntries(_opCtx.get(),
                                                    _sourceNss,
                                                    _targetNssDifferentDb,
                                                    &_opObserver->oplogEntries,
                                                    forApplyOps,
                                                    {});
}

TEST_F(RenameCollectionTest, RenameCollectionAcrossDatabaseDropsTemporaryCollectionOnException) {
    _createCollection(_opCtx.get(), _sourceNss);
    _createIndex(_opCtx.get(), _sourceNss, "a_1");
    _insertDocument(_opCtx.get(), _sourceNss, BSON("_id" << 0));
    _opObserver->onInsertsThrows = true;
    _opObserver->oplogEntries.clear();
    ASSERT_THROWS_CODE(
        renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}).ignore(),
        AssertionException,
        ErrorCodes::OperationFailed);
    _checkOplogEntries(_opObserver->oplogEntries, {"create", "index", "drop"});
}

TEST_F(RenameCollectionTest,
       RenameCollectionAcrossDatabaseDowngradesGlobalWriteLockToNonExclusive) {
    _createCollection(_opCtx.get(), _sourceNss);
    _insertDocument(_opCtx.get(), _sourceNss, BSON("_id" << 0));
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}));
    ASSERT_FALSE(_opObserver->onInsertsIsGlobalWriteLockExclusive);
}

TEST_F(RenameCollectionTest,
       RenameCollectionAcrossDatabaseKeepsGlobalWriteLockExclusiveIfCallerHasGlobalWriteLock) {
    // This simulates the case when renameCollection is called using the applyOps command (different
    // from secondary oplog application).
    _createCollection(_opCtx.get(), _sourceNss);
    _insertDocument(_opCtx.get(), _sourceNss, BSON("_id" << 0));
    Lock::GlobalWrite globalWrite(_opCtx.get());
    ASSERT_OK(renameCollection(_opCtx.get(), _sourceNss, _targetNssDifferentDb, {}));
    ASSERT_TRUE(_opObserver->onInsertsIsGlobalWriteLockExclusive);
}

TEST_F(RenameCollectionTest, FailRenameCollectionFromReplicatedToUnreplicatedDB) {
    NamespaceString sourceNss("foo.isReplicated");
    NamespaceString targetNss("local.isUnreplicated");

    _createCollection(_opCtx.get(), sourceNss);

    ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                  renameCollection(_opCtx.get(), sourceNss, targetNss, {}));
}

TEST_F(RenameCollectionTest, FailRenameCollectionFromUnreplicatedToReplicatedDB) {
    NamespaceString sourceNss("foo.system.profile");
    NamespaceString targetNss("foo.bar");

    _createCollection(_opCtx.get(), sourceNss);

    ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                  renameCollection(_opCtx.get(), sourceNss, targetNss, {}));
}

TEST_F(RenameCollectionTest,
       RenameCollectionForApplyOpsReturnsInvalidNamespaceIfTargetNamespaceIsInvalid) {
    _createCollection(_opCtx.get(), _sourceNss);
    auto dbName = _sourceNss.db().toString();

    // Create a namespace that is not in the form "database.collection".
    NamespaceString invalidTargetNss("invalidNamespace");

    auto cmd = BSON("renameCollection" << _sourceNss.ns() << "to" << invalidTargetNss.ns());

    ASSERT_EQUALS(ErrorCodes::InvalidNamespace,
                  renameCollectionForApplyOps(_opCtx.get(), dbName, {}, cmd, {}));
}

}  // namespace
