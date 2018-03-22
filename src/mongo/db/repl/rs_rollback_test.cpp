/**
 *    Copyright 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <list>
#include <utility>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/rollback_source.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;
using namespace mongo::repl::rollback_internal;

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

const OplogInterfaceMock::Operations kEmptyMockOperations;

ReplSettings createReplSettings() {
    ReplSettings settings;
    settings.setOplogSizeBytes(5 * 1024 * 1024);
    settings.setReplSetString("mySet/node1:12345");
    return settings;
}

class ReplicationCoordinatorRollbackMock : public ReplicationCoordinatorMock {
public:
    ReplicationCoordinatorRollbackMock();
    ReplicationCoordinatorRollbackMock(ServiceContext* service)
        : ReplicationCoordinatorMock(createReplSettings()) {}
    void resetLastOpTimesFromOplog(OperationContext* opCtx) override {}
    bool setFollowerMode(const MemberState& newState) override {
        if (newState == _failSetFollowerModeOnThisMemberState) {
            return false;
        }
        return ReplicationCoordinatorMock::setFollowerMode(newState);
    }
    MemberState _failSetFollowerModeOnThisMemberState = MemberState::RS_UNKNOWN;
};

ReplicationCoordinatorRollbackMock::ReplicationCoordinatorRollbackMock()
    : ReplicationCoordinatorMock(createReplSettings()) {}

class RollbackSourceMock : public RollbackSource {
public:
    RollbackSourceMock(std::unique_ptr<OplogInterface> oplog);
    int getRollbackId() const override;
    const OplogInterface& getOplog() const override;
    BSONObj getLastOperation() const override;
    BSONObj findOne(const NamespaceString& nss, const BSONObj& filter) const override;
    void copyCollectionFromRemote(OperationContext* txn, const NamespaceString& nss) const override;
    StatusWith<BSONObj> getCollectionInfo(const NamespaceString& nss) const override;

private:
    std::unique_ptr<OplogInterface> _oplog;
};

RollbackSourceMock::RollbackSourceMock(std::unique_ptr<OplogInterface> oplog)
    : _oplog(std::move(oplog)) {}

const OplogInterface& RollbackSourceMock::getOplog() const {
    return *_oplog;
}

int RollbackSourceMock::getRollbackId() const {
    return 0;
}

BSONObj RollbackSourceMock::getLastOperation() const {
    auto iter = _oplog->makeIterator();
    auto result = iter->next();
    ASSERT_OK(result.getStatus());
    return result.getValue().first;
}

BSONObj RollbackSourceMock::findOne(const NamespaceString& nss, const BSONObj& filter) const {
    return BSONObj();
}

void RollbackSourceMock::copyCollectionFromRemote(OperationContext* txn,
                                                  const NamespaceString& nss) const {}

StatusWith<BSONObj> RollbackSourceMock::getCollectionInfo(const NamespaceString& nss) const {
    return BSON("name" << nss.ns() << "options" << BSONObj());
}

class RSRollbackTest : public ServiceContextMongoDTest {
protected:
    ServiceContext::UniqueOperationContext _opCtx;

    // Owned by service context
    ReplicationCoordinatorRollbackMock* _coordinator;

    repl::StorageInterfaceMock _storageInterface;

private:
    void setUp() override;
    void tearDown() override;
};

void RSRollbackTest::setUp() {
    ServiceContextMongoDTest::setUp();
    _opCtx = cc().makeOperationContext();
    _coordinator = new ReplicationCoordinatorRollbackMock();

    auto serviceContext = getServiceContext();
    ReplicationCoordinator::set(serviceContext,
                                std::unique_ptr<ReplicationCoordinator>(_coordinator));

    setOplogCollectionName();
    _storageInterface.setAppliedThrough(_opCtx.get(), OpTime{});
    _storageInterface.setMinValid(_opCtx.get(), OpTime{});
}

void RSRollbackTest::tearDown() {
    _opCtx.reset();
    ServiceContextMongoDTest::tearDown();
    setGlobalReplicationCoordinator(nullptr);
}

OplogInterfaceMock::Operation makeNoopOplogEntryAndRecordId(Seconds seconds) {
    OpTime ts(Timestamp(seconds, 0), 0);
    return std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId(1));
}

TEST_F(RSRollbackTest, InconsistentMinValid) {
    _storageInterface.setAppliedThrough(_opCtx.get(), OpTime(Timestamp(Seconds(0), 0), 0));
    _storageInterface.setMinValid(_opCtx.get(), OpTime(Timestamp(Seconds(1), 0), 0));
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock(kEmptyMockOperations),
                               RollbackSourceMock(std::unique_ptr<OplogInterface>(
                                   new OplogInterfaceMock(kEmptyMockOperations))),
                               {},
                               _coordinator,
                               &_storageInterface);
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_EQUALS(18752, status.location());
}

TEST_F(RSRollbackTest, OplogStartMissing) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation =
        std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId());
    ASSERT_EQUALS(
        ErrorCodes::OplogStartMissing,
        syncRollback(_opCtx.get(),
                     OplogInterfaceMock(kEmptyMockOperations),
                     RollbackSourceMock(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                         operation,
                     }))),
                     {},
                     _coordinator,
                     &_storageInterface)
            .code());
}

TEST_F(RSRollbackTest, NoRemoteOpLog) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation =
        std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId());
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock({operation}),
                               RollbackSourceMock(std::unique_ptr<OplogInterface>(
                                   new OplogInterfaceMock(kEmptyMockOperations))),
                               {},
                               _coordinator,
                               &_storageInterface);
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_EQUALS(18752, status.location());
}

TEST_F(RSRollbackTest, RemoteGetRollbackIdThrows) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation =
        std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId());
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)) {}
        int getRollbackId() const override {
            uassert(ErrorCodes::UnknownError, "getRollbackId() failed", false);
        }
    };
    ASSERT_THROWS_CODE(syncRollback(_opCtx.get(),
                                    OplogInterfaceMock({operation}),
                                    RollbackSourceLocal(std::unique_ptr<OplogInterface>(
                                        new OplogInterfaceMock(kEmptyMockOperations))),
                                    {},
                                    _coordinator,
                                    &_storageInterface),
                       UserException,
                       ErrorCodes::UnknownError);
}

TEST_F(RSRollbackTest, RemoteGetRollbackIdDiffersFromRequiredRBID) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation =
        std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId());

    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        using RollbackSourceMock::RollbackSourceMock;
        int getRollbackId() const override {
            return 2;
        }
    };

    ASSERT_THROWS_CODE(syncRollback(_opCtx.get(),
                                    OplogInterfaceMock({operation}),
                                    RollbackSourceLocal(std::unique_ptr<OplogInterface>(
                                        new OplogInterfaceMock(kEmptyMockOperations))),
                                    1,
                                    _coordinator,
                                    &_storageInterface),
                       UserException,
                       ErrorCodes::Error(40362));
}

TEST_F(RSRollbackTest, BothOplogsAtCommonPoint) {
    createOplog(_opCtx.get());
    OpTime ts(Timestamp(Seconds(1), 0), 1);
    auto operation =
        std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId(1));
    ASSERT_OK(
        syncRollback(_opCtx.get(),
                     OplogInterfaceMock({operation}),
                     RollbackSourceMock(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                         operation,
                     }))),
                     {},
                     _coordinator,
                     &_storageInterface));
}

/**
 * Create test collection.
 * Returns collection.
 */
Collection* _createCollection(OperationContext* txn,
                              const NamespaceString& nss,
                              const CollectionOptions& options) {
    Lock::DBLock dbLock(txn->lockState(), nss.db(), MODE_X);
    mongo::WriteUnitOfWork wuow(txn);
    auto db = dbHolder().openDb(txn, nss.db());
    ASSERT_TRUE(db);
    db->dropCollection(txn, nss.ns());
    auto coll = db->createCollection(txn, nss.ns(), options);
    ASSERT_TRUE(coll);
    wuow.commit();
    return coll;
}

Collection* _createCollection(OperationContext* txn,
                              const std::string& nss,
                              const CollectionOptions& options) {
    return _createCollection(txn, NamespaceString(nss), options);
}

/**
 * Test function to roll back a delete operation.
 * Returns number of records in collection after rolling back delete operation.
 * If collection does not exist after rolling back, returns -1.
 */
int _testRollbackDelete(OperationContext* txn,
                        ReplicationCoordinator* coordinator,
                        StorageInterface* storageInterface,
                        const BSONObj& documentAtSource) {
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto deleteOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "d"
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("_id" << 0)),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(const BSONObj& documentAtSource, std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)),
              called(false),
              _documentAtSource(documentAtSource) {}
        BSONObj findOne(const NamespaceString& nss, const BSONObj& filter) const {
            called = true;
            return _documentAtSource;
        }
        mutable bool called;

    private:
        BSONObj _documentAtSource;
    };
    RollbackSourceLocal rollbackSource(documentAtSource,
                                       std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                                           commonOperation,
                                       })));
    ASSERT_OK(syncRollback(txn,
                           OplogInterfaceMock({deleteOperation, commonOperation}),
                           rollbackSource,
                           {},
                           coordinator,
                           storageInterface));
    ASSERT_TRUE(rollbackSource.called);

    Lock::DBLock dbLock(txn->lockState(), "test", MODE_S);
    Lock::CollectionLock collLock(txn->lockState(), "test.t", MODE_S);
    auto db = dbHolder().get(txn, "test");
    ASSERT_TRUE(db);
    auto collection = db->getCollection("test.t");
    if (!collection) {
        return -1;
    }
    return collection->getRecordStore()->numRecords(txn);
}

TEST_F(RSRollbackTest, RollbackDeleteNoDocumentAtSourceCollectionDoesNotExist) {
    createOplog(_opCtx.get());
    ASSERT_EQUALS(-1,
                  _testRollbackDelete(_opCtx.get(), _coordinator, &_storageInterface, BSONObj()));
}

TEST_F(RSRollbackTest, RollbackDeleteNoDocumentAtSourceCollectionExistsNonCapped) {
    createOplog(_opCtx.get());
    _createCollection(_opCtx.get(), "test.t", CollectionOptions());
    _testRollbackDelete(_opCtx.get(), _coordinator, &_storageInterface, BSONObj());
    ASSERT_EQUALS(0,
                  _testRollbackDelete(_opCtx.get(), _coordinator, &_storageInterface, BSONObj()));
}

TEST_F(RSRollbackTest, RollbackDeleteNoDocumentAtSourceCollectionExistsCapped) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.capped = true;
    _createCollection(_opCtx.get(), "test.t", options);
    ASSERT_EQUALS(0,
                  _testRollbackDelete(_opCtx.get(), _coordinator, &_storageInterface, BSONObj()));
}

TEST_F(RSRollbackTest, RollbackDeleteRestoreDocument) {
    createOplog(_opCtx.get());
    _createCollection(_opCtx.get(), "test.t", CollectionOptions());
    BSONObj doc = BSON("_id" << 0 << "a" << 1);
    _testRollbackDelete(_opCtx.get(), _coordinator, &_storageInterface, doc);
    ASSERT_EQUALS(1, _testRollbackDelete(_opCtx.get(), _coordinator, &_storageInterface, doc));
}

TEST_F(RSRollbackTest, RollbackInsertDocumentWithNoId) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto insertDocumentOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "i"
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("a" << 1)),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        BSONObj findOne(const NamespaceString& nss, const BSONObj& filter) const {
            called = true;
            return BSONObj();
        }
        mutable bool called;

    private:
        BSONObj _documentAtSource;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock({insertDocumentOperation, commonOperation}),
                               rollbackSource,
                               {},
                               _coordinator,
                               &_storageInterface);
    stopCapturingLogMessages();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_EQUALS(18752, status.location());
    ASSERT_EQUALS(1, countLogLinesContaining("cannot rollback op with no _id. ns: test.t,"));
    ASSERT_FALSE(rollbackSource.called);
}

TEST_F(RSRollbackTest, RollbackCreateIndexCommand) {
    createOplog(_opCtx.get());
    auto collection = _createCollection(_opCtx.get(), "test.t", CollectionOptions());
    auto indexSpec = BSON("ns"
                          << "test.t"
                          << "key"
                          << BSON("a" << 1)
                          << "name"
                          << "a_1"
                          << "v"
                          << static_cast<int>(kIndexVersion));
    {
        Lock::DBLock dbLock(_opCtx->lockState(), "test", MODE_X);
        MultiIndexBlock indexer(_opCtx.get(), collection);
        ASSERT_OK(indexer.init(indexSpec).getStatus());
        WriteUnitOfWork wunit(_opCtx.get());
        indexer.commit();
        wunit.commit();
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(2, indexCatalog->numIndexesReady(_opCtx.get()));
    }
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto insertDocumentOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "i"
                                 << "ns"
                                 << "test.system.indexes"
                                 << "o"
                                 << indexSpec),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        void copyCollectionFromRemote(OperationContext* txn,
                                      const NamespaceString& nss) const override {
            called = true;
        }
        mutable bool called;

    private:
        BSONObj _documentAtSource;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    // Repeat index creation operation and confirm that rollback attempts to drop index just once.
    // This can happen when an index is re-created with different options.
    startCapturingLogMessages();
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({insertDocumentOperation, insertDocumentOperation, commonOperation}),
        rollbackSource,
        {},
        _coordinator,
        &_storageInterface));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countLogLinesContaining("rollback drop index: collection: test.t. index: a_1"));
    ASSERT_FALSE(rollbackSource.called);
    {
        Lock::DBLock dbLock(_opCtx->lockState(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }
}

TEST_F(RSRollbackTest, RollbackCreateIndexCommandIndexNotInCatalog) {
    createOplog(_opCtx.get());
    auto collection = _createCollection(_opCtx.get(), "test.t", CollectionOptions());
    auto indexSpec = BSON("ns"
                          << "test.t"
                          << "key"
                          << BSON("a" << 1)
                          << "name"
                          << "a_1");
    // Skip index creation to trigger warning during rollback.
    {
        Lock::DBLock dbLock(_opCtx->lockState(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto insertDocumentOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "i"
                                 << "ns"
                                 << "test.system.indexes"
                                 << "o"
                                 << indexSpec),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        void copyCollectionFromRemote(OperationContext* txn,
                                      const NamespaceString& nss) const override {
            called = true;
        }
        mutable bool called;

    private:
        BSONObj _documentAtSource;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({insertDocumentOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           &_storageInterface));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countLogLinesContaining("rollback drop index: collection: test.t. index: a_1"));
    ASSERT_EQUALS(1, countLogLinesContaining("rollback failed to drop index a_1 in test.t"));
    ASSERT_FALSE(rollbackSource.called);
    {
        Lock::DBLock dbLock(_opCtx->lockState(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }
}

TEST_F(RSRollbackTest, RollbackCreateIndexCommandMissingNamespace) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto insertDocumentOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "i"
                                 << "ns"
                                 << "test.system.indexes"
                                 << "o"
                                 << BSON("key" << BSON("a" << 1) << "name"
                                               << "a_1")),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        void copyCollectionFromRemote(OperationContext* txn,
                                      const NamespaceString& nss) const override {
            called = true;
        }
        mutable bool called;

    private:
        BSONObj _documentAtSource;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock({insertDocumentOperation, commonOperation}),
                               rollbackSource,
                               {},
                               _coordinator,
                               &_storageInterface);
    stopCapturingLogMessages();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_EQUALS(18752, status.location());
    ASSERT_EQUALS(
        1, countLogLinesContaining("Missing collection namespace in system.indexes operation,"));
    ASSERT_FALSE(rollbackSource.called);
}

TEST_F(RSRollbackTest, RollbackCreateIndexCommandInvalidNamespace) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto insertDocumentOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "i"
                                 << "ns"
                                 << "test.system.indexes"
                                 << "o"
                                 << BSON("ns"
                                         << "test."
                                         << "key"
                                         << BSON("a" << 1)
                                         << "name"
                                         << "a_1")),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        void copyCollectionFromRemote(OperationContext* txn,
                                      const NamespaceString& nss) const override {
            called = true;
        }
        mutable bool called;

    private:
        BSONObj _documentAtSource;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock({insertDocumentOperation, commonOperation}),
                               rollbackSource,
                               {},
                               _coordinator,
                               &_storageInterface);
    stopCapturingLogMessages();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_EQUALS(18752, status.location());
    ASSERT_EQUALS(
        1, countLogLinesContaining("Invalid collection namespace in system.indexes operation,"));
    ASSERT_FALSE(rollbackSource.called);
}

TEST_F(RSRollbackTest, RollbackCreateIndexCommandMissingIndexName) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto insertDocumentOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "i"
                                 << "ns"
                                 << "test.system.indexes"
                                 << "o"
                                 << BSON("ns"
                                         << "test.t"
                                         << "key"
                                         << BSON("a" << 1))),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        void copyCollectionFromRemote(OperationContext* txn,
                                      const NamespaceString& nss) const override {
            called = true;
        }
        mutable bool called;

    private:
        BSONObj _documentAtSource;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock({insertDocumentOperation, commonOperation}),
                               rollbackSource,
                               {},
                               _coordinator,
                               &_storageInterface);
    stopCapturingLogMessages();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_EQUALS(18752, status.location());
    ASSERT_EQUALS(1, countLogLinesContaining("Missing index name in system.indexes operation,"));
    ASSERT_FALSE(rollbackSource.called);
}

TEST_F(RSRollbackTest, RollbackUnknownCommand) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto unknownCommandOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("unknown_command"
                                         << "t")),
                       RecordId(2));
    {
        Lock::DBLock dbLock(_opCtx->lockState(), "test", MODE_X);
        mongo::WriteUnitOfWork wuow(_opCtx.get());
        auto db = dbHolder().openDb(_opCtx.get(), "test");
        ASSERT_TRUE(db);
        ASSERT_TRUE(db->getOrCreateCollection(_opCtx.get(), "test.t"));
        wuow.commit();
    }
    auto status =
        syncRollback(_opCtx.get(),
                     OplogInterfaceMock({unknownCommandOperation, commonOperation}),
                     RollbackSourceMock(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                         commonOperation,
                     }))),
                     {},
                     _coordinator,
                     &_storageInterface);
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_EQUALS(18751, status.location());
}

TEST_F(RSRollbackTest, RollbackDropCollectionCommand) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto dropCollectionOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("drop"
                                         << "t")),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        void copyCollectionFromRemote(OperationContext* txn,
                                      const NamespaceString& nss) const override {
            called = true;
        }
        mutable bool called;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    _createCollection(_opCtx.get(), "test.t", CollectionOptions());
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({dropCollectionOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           &_storageInterface));
    ASSERT_TRUE(rollbackSource.called);
}

TEST_F(RSRollbackTest, RollbackDropCollectionCommandFailsIfRBIDChangesWhileSyncingCollection) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto dropCollectionOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("drop"
                                         << "t")),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        using RollbackSourceMock::RollbackSourceMock;
        int getRollbackId() const override {
            return copyCollectionCalled ? 1 : 0;
        }
        void copyCollectionFromRemote(OperationContext* txn,
                                      const NamespaceString& nss) const override {
            copyCollectionCalled = true;
        }
        mutable bool copyCollectionCalled = false;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    _createCollection(_opCtx.get(), "test.t", CollectionOptions());
    ASSERT_THROWS_CODE(syncRollback(_opCtx.get(),
                                    OplogInterfaceMock({dropCollectionOperation, commonOperation}),
                                    rollbackSource,
                                    0,
                                    _coordinator,
                                    &_storageInterface),
                       DBException,
                       40365);
    ASSERT(rollbackSource.copyCollectionCalled);
}

BSONObj makeApplyOpsOplogEntry(Timestamp ts, std::initializer_list<BSONObj> ops) {
    BSONObjBuilder entry;
    entry << "ts" << ts << "h" << 1LL << "op"
          << "c"
          << "ns"
          << "admin";
    {
        BSONObjBuilder cmd(entry.subobjStart("o"));
        BSONArrayBuilder subops(entry.subarrayStart("applyOps"));
        for (const auto& op : ops) {
            subops << op;
        }
    }
    return entry.obj();
}

OpTime getOpTimeFromOplogEntry(const BSONObj& entry) {
    const BSONElement tsElement = entry["ts"];
    const BSONElement termElement = entry["t"];
    const BSONElement hashElement = entry["h"];
    ASSERT_EQUALS(bsonTimestamp, tsElement.type()) << entry;
    ASSERT_TRUE(hashElement.isNumber()) << entry;
    ASSERT_TRUE(termElement.eoo() || termElement.isNumber()) << entry;
    long long term = hashElement.numberLong();
    if (!termElement.eoo()) {
        term = termElement.numberLong();
    }
    return OpTime(tsElement.timestamp(), term);
}

TEST_F(RSRollbackTest, RollbackApplyOpsCommand) {
    createOplog(_opCtx.get());

    {
        AutoGetOrCreateDb autoDb(_opCtx.get(), "test", MODE_X);
        mongo::WriteUnitOfWork wuow(_opCtx.get());
        auto coll = autoDb.getDb()->getCollection("test.t");
        if (!coll) {
            coll = autoDb.getDb()->createCollection(_opCtx.get(), "test.t");
        }
        ASSERT(coll);
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(
            coll->insertDocument(_opCtx.get(), BSON("_id" << 1 << "v" << 2), nullOpDebug, false));
        ASSERT_OK(
            coll->insertDocument(_opCtx.get(), BSON("_id" << 2 << "v" << 4), nullOpDebug, false));
        ASSERT_OK(coll->insertDocument(_opCtx.get(), BSON("_id" << 4), nullOpDebug, false));
        wuow.commit();
    }
    const auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    const auto applyOpsOperation =
        std::make_pair(makeApplyOpsOplogEntry(Timestamp(Seconds(2), 0),
                                              {BSON("op"
                                                    << "u"
                                                    << "ns"
                                                    << "test.t"
                                                    << "o2"
                                                    << BSON("_id" << 1)
                                                    << "o"
                                                    << BSON("_id" << 1 << "v" << 2)),
                                               BSON("op"
                                                    << "u"
                                                    << "ns"
                                                    << "test.t"
                                                    << "o2"
                                                    << BSON("_id" << 2)
                                                    << "o"
                                                    << BSON("_id" << 2 << "v" << 4)),
                                               BSON("op"
                                                    << "d"
                                                    << "ns"
                                                    << "test.t"
                                                    << "o"
                                                    << BSON("_id" << 3)),
                                               BSON("op"
                                                    << "i"
                                                    << "ns"
                                                    << "test.t"
                                                    << "o"
                                                    << BSON("_id" << 4))}),
                       RecordId(2));

    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)) {}

        BSONObj findOne(const NamespaceString& nss, const BSONObj& filter) const override {
            int numFields = 0;
            for (const auto element : filter) {
                ++numFields;
                ASSERT_EQUALS("_id", element.fieldNameStringData()) << filter;
            }
            ASSERT_EQUALS(1, numFields) << filter;
            searchedIds.insert(filter.firstElement().numberInt());
            switch (filter.firstElement().numberInt()) {
                case 1:
                    return BSON("_id" << 1 << "v" << 1);
                case 2:
                    return BSON("_id" << 2 << "v" << 3);
                case 3:
                    return BSON("_id" << 3 << "v" << 5);
                case 4:
                    return {};
            }
            FAIL("Unexpected findOne request") << filter;
            return {};  // Unreachable; why doesn't compiler know?
        }

        mutable std::multiset<int> searchedIds;
    } rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})));

    _createCollection(_opCtx.get(), "test.t", CollectionOptions());
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({applyOpsOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           &_storageInterface));
    ASSERT_EQUALS(4U, rollbackSource.searchedIds.size());
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(1));
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(2));
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(3));
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(4));

    AutoGetCollectionForRead acr(_opCtx.get(), "test.t");
    BSONObj result;
    ASSERT(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 1), result));
    ASSERT_EQUALS(1, result["v"].numberInt()) << result;
    ASSERT(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 2), result));
    ASSERT_EQUALS(3, result["v"].numberInt()) << result;
    ASSERT(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 3), result));
    ASSERT_EQUALS(5, result["v"].numberInt()) << result;
    ASSERT_FALSE(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 4), result))
        << result;
}

TEST_F(RSRollbackTest, RollbackCreateCollectionCommand) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto createCollectionOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("create"
                                         << "t")),
                       RecordId(2));
    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    _createCollection(_opCtx.get(), "test.t", CollectionOptions());
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({createCollectionOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           &_storageInterface));
    {
        Lock::DBLock dbLock(_opCtx->lockState(), "test", MODE_S);
        auto db = dbHolder().get(_opCtx.get(), "test");
        ASSERT_TRUE(db);
        ASSERT_FALSE(db->getCollection("test.t"));
    }
}

TEST_F(RSRollbackTest, RollbackCollectionModificationCommand) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto collectionModificationOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("collMod"
                                         << "t"
                                         << "noPadding"
                                         << false)),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        StatusWith<BSONObj> getCollectionInfo(const NamespaceString& nss) const {
            called = true;
            return RollbackSourceMock::getCollectionInfo(nss);
        }
        mutable bool called;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    _createCollection(_opCtx.get(), "test.t", CollectionOptions());
    startCapturingLogMessages();
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({collectionModificationOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           &_storageInterface));
    stopCapturingLogMessages();
    ASSERT_TRUE(rollbackSource.called);
    for (const auto& message : getCapturedLogMessages()) {
        ASSERT_TRUE(message.find("ignoring op with no _id during rollback. ns: test.t") ==
                    std::string::npos);
    }
}

TEST_F(RSRollbackTest, RollbackCollectionModificationCommandInvalidCollectionOptions) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto collectionModificationOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("collMod"
                                         << "t"
                                         << "noPadding"
                                         << false)),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)) {}
        StatusWith<BSONObj> getCollectionInfo(const NamespaceString& nss) const {
            return BSON("name" << nss.ns() << "options" << 12345);
        }
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    _createCollection(_opCtx.get(), "test.t", CollectionOptions());
    auto status =
        syncRollback(_opCtx.get(),
                     OplogInterfaceMock({collectionModificationOperation, commonOperation}),
                     rollbackSource,
                     {},
                     _coordinator,
                     &_storageInterface);
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_EQUALS(18753, status.location());
}

TEST(RSRollbackTest, LocalEntryWithoutNsIsFatal) {
    const auto validOplogEntry = fromjson("{op: 'i', ns: 'test.t', o: {_id:1, a: 1}}");
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(fui, validOplogEntry));
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(fui, validOplogEntry.removeField("ns")),
                  RSFatalException);
}

TEST(RSRollbackTest, LocalEntryWithoutOIsFatal) {
    const auto validOplogEntry = fromjson("{op: 'i', ns: 'test.t', o: {_id:1, a: 1}}");
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(fui, validOplogEntry));
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(fui, validOplogEntry.removeField("o")),
                  RSFatalException);
}

TEST(RSRollbackTest, LocalEntryWithoutO2IsFatal) {
    const auto validOplogEntry =
        fromjson("{op: 'u', ns: 'test.t', o2: {_id: 1}, o: {_id:1, a: 1}}");
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(fui, validOplogEntry));
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(fui, validOplogEntry.removeField("o2")),
                  RSFatalException);
}

TEST_F(RSRollbackTest, RollbackReturnsImmediatelyOnFailureToTransitionToRollback) {
    // On failing to transition to ROLLBACK, rollback() should return immediately and not call
    // syncRollback(). We provide an empty oplog so that if syncRollback() is called erroneously,
    // we would go fatal.
    OplogInterfaceMock localOplogWithSingleOplogEntry({makeNoopOplogEntryAndRecordId(Seconds(1))});
    RollbackSourceMock rollbackSourceWithInvalidOplog(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock(kEmptyMockOperations)));

    // Inject ReplicationCoordinator::setFollowerMode() error. We set the current member state
    // because it will be logged by rollback() on failing to transition to ROLLBACK.
    _coordinator->setFollowerMode(MemberState::RS_SECONDARY);
    _coordinator->_failSetFollowerModeOnThisMemberState = MemberState::RS_ROLLBACK;

    startCapturingLogMessages();
    rollback(_opCtx.get(),
             localOplogWithSingleOplogEntry,
             rollbackSourceWithInvalidOplog,
             {},
             _coordinator,
             &_storageInterface);
    stopCapturingLogMessages();

    ASSERT_EQUALS(1, countLogLinesContaining("Cannot transition from SECONDARY to ROLLBACK"));
    ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY), _coordinator->getMemberState());
}

DEATH_TEST_F(RSRollbackTest,
             RollbackUnrecoverableRollbackErrorTriggersFatalAssertion,
             "Unable to complete rollback. A full resync may be needed: "
             "UnrecoverableRollbackError: need to rollback, but unable to determine common point "
             "between local and remote oplog: InvalidSyncSource: remote oplog empty or unreadable "
             "@ 18752") {
    // rollback() should abort on getting UnrecoverableRollbackError from syncRollback(). An empty
    // local oplog will make syncRollback() return the intended error.
    OplogInterfaceMock localOplogWithSingleOplogEntry({makeNoopOplogEntryAndRecordId(Seconds(1))});
    RollbackSourceMock rollbackSourceWithInvalidOplog(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock(kEmptyMockOperations)));

    rollback(_opCtx.get(),
             localOplogWithSingleOplogEntry,
             rollbackSourceWithInvalidOplog,
             {},
             _coordinator,
             &_storageInterface);
}

TEST_F(RSRollbackTest, RollbackLogsRetryMessageAndReturnsOnNonUnrecoverableRollbackError) {
    // If local oplog is empty, syncRollback() returns OplogStartMissing (instead of
    // UnrecoverableRollbackError when the remote oplog is missing). rollback() should log a message
    // about retrying rollback later before returning.
    OplogInterfaceMock localOplogWithNoEntries(kEmptyMockOperations);
    RollbackSourceMock rollbackSourceWithValidOplog(std::unique_ptr<OplogInterface>(
        new OplogInterfaceMock({makeNoopOplogEntryAndRecordId(Seconds(1))})));
    auto noopSleepSecsFn = [](int) {};

    startCapturingLogMessages();
    rollback(_opCtx.get(),
             localOplogWithNoEntries,
             rollbackSourceWithValidOplog,
             {},
             _coordinator,
             &_storageInterface,
             noopSleepSecsFn);
    stopCapturingLogMessages();

    ASSERT_EQUALS(
        1, countLogLinesContaining("rollback cannot complete at this time (retrying later)"));
    ASSERT_EQUALS(MemberState(MemberState::RS_RECOVERING), _coordinator->getMemberState());
}

DEATH_TEST_F(RSRollbackTest,
             RollbackTriggersFatalAssertionOnDetectingShardIdentityDocumentRollback,
             "shardIdentity document rollback detected.  Shutting down to clear in-memory sharding "
             "state.  Restarting this process should safely return it to a healthy state") {
    auto commonOperation = makeNoopOplogEntryAndRecordId(Seconds(1));
    OplogInterfaceMock localOplog({commonOperation});
    RollbackSourceMock rollbackSource(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})));

    ASSERT_FALSE(ShardIdentityRollbackNotifier::get(_opCtx.get())->didRollbackHappen());
    ShardIdentityRollbackNotifier::get(_opCtx.get())->recordThatRollbackHappened();
    ASSERT_TRUE(ShardIdentityRollbackNotifier::get(_opCtx.get())->didRollbackHappen());

    createOplog(_opCtx.get());
    rollback(_opCtx.get(), localOplog, rollbackSource, {}, _coordinator, &_storageInterface);
}

DEATH_TEST_F(
    RSRollbackTest,
    RollbackTriggersFatalAssertionOnFailingToTransitionToRecoveringAfterSyncRollbackReturns,
    "Failed to transition into RECOVERING; expected to be in state ROLLBACK but found self in "
    "ROLLBACK") {
    auto commonOperation = makeNoopOplogEntryAndRecordId(Seconds(1));
    OplogInterfaceMock localOplog({commonOperation});
    RollbackSourceMock rollbackSource(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})));

    _coordinator->_failSetFollowerModeOnThisMemberState = MemberState::RS_RECOVERING;

    createOplog(_opCtx.get());
    rollback(_opCtx.get(), localOplog, rollbackSource, {}, _coordinator, &_storageInterface);
}

// The testcases used here are trying to detect off-by-one errors in
// FixUpInfo::removeAllDocsToRefectchFor.
TEST(FixUpInfoTest, RemoveAllDocsToRefetchForWorks) {
    const auto normalHolder = BSON("" << OID::gen());
    const auto normalKey = normalHolder.firstElement();

    // Can't use ASSERT_EQ with this since it isn't ostream-able. Failures will at least give you
    // the size. If that isn't enough, use GDB.
    using DocSet = std::set<DocID>;

    FixUpInfo fui;
    fui.docsToRefetch = {
        DocID::minFor("a"),
        DocID{{}, "a", normalKey},
        DocID::maxFor("a"),

        DocID::minFor("b"),
        DocID{{}, "b", normalKey},
        DocID::maxFor("b"),

        DocID::minFor("c"),
        DocID{{}, "c", normalKey},
        DocID::maxFor("c"),
    };

    // Remove from the middle.
    fui.removeAllDocsToRefetchFor("b");
    ASSERT((fui.docsToRefetch ==
            DocSet{
                DocID::minFor("a"),
                DocID{{}, "a", normalKey},
                DocID::maxFor("a"),

                DocID::minFor("c"),
                DocID{{}, "c", normalKey},
                DocID::maxFor("c"),
            }))
        << "remaining docs: " << fui.docsToRefetch.size();

    // Remove from the end.
    fui.removeAllDocsToRefetchFor("c");
    ASSERT((fui.docsToRefetch ==
            DocSet{
                DocID::minFor("a"),  // This comment helps clang-format.
                DocID{{}, "a", normalKey},
                DocID::maxFor("a"),
            }))
        << "remaining docs: " << fui.docsToRefetch.size();

    // Everything else.
    fui.removeAllDocsToRefetchFor("a");
    ASSERT((fui.docsToRefetch == DocSet{})) << "remaining docs: " << fui.docsToRefetch.size();
}

}  // namespace
