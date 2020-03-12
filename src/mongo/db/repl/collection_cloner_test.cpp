
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

#include <memory>
#include <vector>

#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/base_cloner_test_fixture.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace {

using namespace mongo;
using namespace mongo::repl;
using namespace unittest;

class MockCallbackState final : public mongo::executor::TaskExecutor::CallbackState {
public:
    MockCallbackState() = default;
    void cancel() override {}
    void waitForCompletion() override {}
    bool isCanceled() const override {
        return false;
    }
};

class CollectionClonerTest : public BaseClonerTest {
public:
    BaseCloner* getCloner() const override;

protected:
    auto setStatusCallback() {
        return [this](const Status& s) { setStatus(s); };
    }

    void setUp() override;
    void tearDown() override;

    std::vector<BSONObj> makeSecondaryIndexSpecs(const NamespaceString& nss);

    // A simple arbitrary value to use as the default batch size.
    const int defaultBatchSize = 1024;

    // Running initial sync with a single cursor will default to using the 'find' command until
    // 'parallelCollectionScan' has more complete testing.
    const int defaultNumCloningCursors = 1;

    CollectionOptions options;
    std::unique_ptr<CollectionCloner> collectionCloner;
    CollectionMockStats collectionStats;  // Used by the _loader.
    CollectionBulkLoaderMock* _loader;    // Owned by CollectionCloner.
};

void CollectionClonerTest::setUp() {
    BaseClonerTest::setUp();
    options = {};
    collectionCloner.reset(nullptr);
    collectionCloner = stdx::make_unique<CollectionCloner>(&getExecutor(),
                                                           dbWorkThreadPool.get(),
                                                           target,
                                                           nss,
                                                           options,
                                                           setStatusCallback(),
                                                           storageInterface.get(),
                                                           defaultBatchSize,
                                                           defaultNumCloningCursors);
    collectionStats = CollectionMockStats();
    storageInterface->createCollectionForBulkFn =
        [this](const NamespaceString& nss,
               const CollectionOptions& options,
               const BSONObj idIndexSpec,
               const std::vector<BSONObj>& nonIdIndexSpecs) {
            (_loader = new CollectionBulkLoaderMock(&collectionStats))
                ->init(nonIdIndexSpecs)
                .transitional_ignore();

            return StatusWith<std::unique_ptr<CollectionBulkLoader>>(
                std::unique_ptr<CollectionBulkLoader>(_loader));
        };
}

// Return index specs to use for secondary indexes.
std::vector<BSONObj> CollectionClonerTest::makeSecondaryIndexSpecs(const NamespaceString& nss) {
    return {BSON("v" << 1 << "key" << BSON("a" << 1) << "name"
                     << "a_1"
                     << "ns"
                     << nss.ns()),
            BSON("v" << 1 << "key" << BSON("b" << 1) << "name"
                     << "b_1"
                     << "ns"
                     << nss.ns())};
}

void CollectionClonerTest::tearDown() {
    BaseClonerTest::tearDown();
    // Executor may still invoke collection cloner's callback before shutting down.
    collectionCloner.reset(nullptr);
    options = {};
}

BaseCloner* CollectionClonerTest::getCloner() const {
    return collectionCloner.get();
}


TEST_F(CollectionClonerTest, InvalidConstruction) {
    executor::TaskExecutor& executor = getExecutor();
    auto pool = dbWorkThreadPool.get();

    const auto& cb = [](const Status&) { FAIL("should not reach here"); };

    // Null executor -- error from Fetcher, not CollectionCloner.
    {
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(CollectionCloner(nullptr,
                                                     pool,
                                                     target,
                                                     nss,
                                                     options,
                                                     cb,
                                                     si,
                                                     defaultBatchSize,
                                                     defaultNumCloningCursors),
                                    AssertionException,
                                    ErrorCodes::BadValue,
                                    "task executor cannot be null");
    }

    // Null storage interface
    ASSERT_THROWS_CODE_AND_WHAT(CollectionCloner(&executor,
                                                 pool,
                                                 target,
                                                 nss,
                                                 options,
                                                 cb,
                                                 nullptr,
                                                 defaultBatchSize,
                                                 defaultNumCloningCursors),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "storage interface cannot be null");

    // Invalid namespace.
    {
        NamespaceString badNss("db.");
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(CollectionCloner(&executor,
                                                     pool,
                                                     target,
                                                     badNss,
                                                     options,
                                                     cb,
                                                     si,
                                                     defaultBatchSize,
                                                     defaultNumCloningCursors),
                                    AssertionException,
                                    ErrorCodes::BadValue,
                                    "invalid collection namespace: db.");
    }

    // Invalid collection options - error from CollectionOptions::validate(), not CollectionCloner.
    {
        CollectionOptions invalidOptions;
        invalidOptions.storageEngine = BSON("storageEngine1"
                                            << "not a document");
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(
            CollectionCloner(&executor,
                             pool,
                             target,
                             nss,
                             invalidOptions,
                             cb,
                             si,
                             defaultBatchSize,
                             defaultNumCloningCursors),
            AssertionException,
            ErrorCodes::BadValue,
            "'storageEngine.storageEngine1' has to be an embedded document.");
    }

    // Callback function cannot be null.
    {
        CollectionCloner::CallbackFn nullCb;
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(CollectionCloner(&executor,
                                                     pool,
                                                     target,
                                                     nss,
                                                     options,
                                                     nullCb,
                                                     si,
                                                     defaultBatchSize,
                                                     defaultNumCloningCursors),
                                    AssertionException,
                                    ErrorCodes::BadValue,
                                    "callback function cannot be null");
    }
}

TEST_F(CollectionClonerTest, ClonerLifeCycle) {
    testLifeCycle();
}

TEST_F(CollectionClonerTest, FirstRemoteCommand) {
    ASSERT_OK(collectionCloner->startup());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
    ASSERT_TRUE(net->hasReadyRequests());
    NetworkOperationIterator noi = net->getNextReadyRequest();
    auto&& noiRequest = noi->getRequest();
    ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
    ASSERT_EQUALS("count", std::string(noiRequest.cmdObj.firstElementFieldName()));
    ASSERT_EQUALS(nss.coll().toString(), noiRequest.cmdObj.firstElement().valuestrsafe());
    ASSERT_FALSE(net->hasReadyRequests());
    ASSERT_TRUE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, CollectionClonerSetsDocumentCountInStatsFromCountCommandResult) {
    ASSERT_OK(collectionCloner->startup());

    ASSERT_EQUALS(0U, collectionCloner->getStats().documentToCopy);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(100));
    }
    getExecutor().shutdown();
    collectionCloner->join();
    ASSERT_EQUALS(100U, collectionCloner->getStats().documentToCopy);
}

TEST_F(CollectionClonerTest, CollectionClonerPassesThroughNonRetriableErrorFromCountCommand) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(ErrorCodes::OperationFailed, "");
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
}

TEST_F(CollectionClonerTest, CollectionClonerPassesThroughCommandStatusErrorFromCountCommand) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                         << "count error"
                                         << "code"
                                         << int(ErrorCodes::OperationFailed)));
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "count error");
}

TEST_F(CollectionClonerTest, CollectionClonerResendsCountCommandOnRetriableError) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(ErrorCodes::HostNotFound, "");
        processNetworkResponse(ErrorCodes::NetworkTimeout, "");
        processNetworkResponse(createCountResponse(100));
    }
    getExecutor().shutdown();
    collectionCloner->join();
    ASSERT_EQUALS(100U, collectionCloner->getStats().documentToCopy);
}

TEST_F(CollectionClonerTest, CollectionClonerReturnsLastRetriableErrorOnExceedingCountAttempts) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(ErrorCodes::HostNotFound, "");
        processNetworkResponse(ErrorCodes::NetworkTimeout, "");
        processNetworkResponse(ErrorCodes::NotMaster, "");
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::NotMaster, getStatus());
}

TEST_F(CollectionClonerTest, CollectionClonerReturnsNoSuchKeyOnMissingDocumentCountFieldName) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("ok" << 1));
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, getStatus());
}

TEST_F(CollectionClonerTest, CollectionClonerDoesNotAbortOnNegativeDocumentCount) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(-1));
    }
    getExecutor().shutdown();
    collectionCloner->join();
    ASSERT_EQUALS(0U, collectionCloner->getStats().documentToCopy);
}

class TaskExecutorWithFailureInScheduleRemoteCommand : public unittest::TaskExecutorProxy {
public:
    using ShouldFailRequestFn = stdx::function<bool(const executor::RemoteCommandRequest&)>;

    TaskExecutorWithFailureInScheduleRemoteCommand(executor::TaskExecutor* executor,
                                                   ShouldFailRequestFn shouldFailRequest)
        : unittest::TaskExecutorProxy(executor), _shouldFailRequest(shouldFailRequest) {}

    StatusWith<CallbackHandle> scheduleRemoteCommand(
        const executor::RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const transport::BatonHandle& baton = nullptr) override {
        if (_shouldFailRequest(request)) {
            return Status(ErrorCodes::OperationFailed, "failed to schedule remote command");
        }
        return getExecutor()->scheduleRemoteCommand(request, cb, baton);
    }

private:
    ShouldFailRequestFn _shouldFailRequest;
};

TEST_F(CollectionClonerTest,
       CollectionClonerReturnsScheduleErrorOnFailingToScheduleListIndexesCommand) {
    TaskExecutorWithFailureInScheduleRemoteCommand _executorProxy(
        &getExecutor(), [](const executor::RemoteCommandRequest& request) {
            return str::equals("listIndexes", request.cmdObj.firstElementFieldName());
        });

    collectionCloner = stdx::make_unique<CollectionCloner>(&_executorProxy,
                                                           dbWorkThreadPool.get(),
                                                           target,
                                                           nss,
                                                           options,
                                                           setStatusCallback(),
                                                           storageInterface.get(),
                                                           defaultBatchSize,
                                                           defaultNumCloningCursors);

    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(100));
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
}

TEST_F(CollectionClonerTest, DoNotCreateIDIndexIfAutoIndexIdUsed) {
    options = {};
    options.autoIndexId = CollectionOptions::NO;
    collectionCloner.reset(new CollectionCloner(&getExecutor(),
                                                dbWorkThreadPool.get(),
                                                target,
                                                nss,
                                                options,
                                                setStatusCallback(),
                                                storageInterface.get(),
                                                defaultBatchSize,
                                                defaultNumCloningCursors));

    NamespaceString collNss;
    CollectionOptions collOptions;
    std::vector<BSONObj> collIndexSpecs{BSON("fakeindexkeys" << 1)};  // init with one doc.
    storageInterface->createCollectionForBulkFn = [&,
                                                   this](const NamespaceString& theNss,
                                                         const CollectionOptions& theOptions,
                                                         const BSONObj idIndexSpec,
                                                         const std::vector<BSONObj>& theIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
            CollectionBulkLoaderMock* loader = new CollectionBulkLoaderMock(&collectionStats);
            collNss = theNss;
            collOptions = theOptions;
            collIndexSpecs = theIndexSpecs;
            loader->init(theIndexSpecs).transitional_ignore();
            return std::unique_ptr<CollectionBulkLoader>(loader);
        };

    ASSERT_OK(collectionCloner->startup());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSONArray()));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, emptyArray));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(BSON_ARRAY(doc)));
    }
    collectionCloner->join();
    ASSERT_EQUALS(1, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_EQ(collOptions.autoIndexId, CollectionOptions::NO);
    ASSERT_EQ(0UL, collIndexSpecs.size());
    ASSERT_EQ(collNss, nss);
}

// A collection may have no indexes. The cloner will produce a warning but
// will still proceed with cloning.
TEST_F(CollectionClonerTest, ListIndexesReturnedNoIndexes) {
    ASSERT_OK(collectionCloner->startup());

    // Using a non-zero cursor to ensure that
    // the cloner stops the fetcher from retrieving more results.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(1, BSONArray()));
    }

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        ASSERT_TRUE(getNet()->hasReadyRequests());
    }
}

TEST_F(CollectionClonerTest, ListIndexesReturnedNamespaceNotFound) {
    ASSERT_OK(collectionCloner->startup());

    bool collectionCreated = false;
    bool writesAreReplicatedOnOpCtx = false;
    NamespaceString collNss;
    storageInterface->createCollFn = [&collNss, &collectionCreated, &writesAreReplicatedOnOpCtx](
        OperationContext* opCtx, const NamespaceString& nss, const CollectionOptions& options) {
        writesAreReplicatedOnOpCtx = opCtx->writesAreReplicated();
        collectionCreated = true;
        collNss = nss;
        return Status::OK();
    };
    // Using a non-zero cursor to ensure that
    // the cloner stops the fetcher from retrieving more results.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(ErrorCodes::NamespaceNotFound, "The collection doesn't exist.");
    }

    collectionCloner->join();
    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_TRUE(collectionCreated);
    ASSERT_FALSE(writesAreReplicatedOnOpCtx);
    ASSERT_EQ(collNss, nss);
}


TEST_F(CollectionClonerTest, CollectionClonerResendsListIndexesCommandOnRetriableError) {
    ASSERT_OK(collectionCloner->startup());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);

    // First request sent by CollectionCloner. CollectionCollection sends listIndexes request
    // irrespective of collection size in a successful count response.
    assertRemoteCommandNameEquals("count", net->scheduleSuccessfulResponse(createCountResponse(0)));
    net->runReadyNetworkOperations();

    // Respond to first listIndexes request with a retriable error.
    assertRemoteCommandNameEquals("listIndexes",
                                  net->scheduleErrorResponse(Status(ErrorCodes::HostNotFound, "")));
    net->runReadyNetworkOperations();
    ASSERT_TRUE(collectionCloner->isActive());

    // Confirm that CollectionCloner resends the listIndexes request.
    auto noi = net->getNextReadyRequest();
    assertRemoteCommandNameEquals("listIndexes", noi->getRequest());
    net->blackHole(noi);
}

TEST_F(CollectionClonerTest,
       ListIndexesReturnedNamespaceNotFoundAndCreateCollectionCallbackCanceled) {
    ASSERT_OK(collectionCloner->startup());

    // Replace scheduleDbWork function to schedule the create collection task with an injected error
    // status.
    auto exec = &getExecutor();
    collectionCloner->setScheduleDbWorkFn_forTest([exec](
        const executor::TaskExecutor::CallbackFn& workFn) {
        auto wrappedTask = [workFn](const executor::TaskExecutor::CallbackArgs& cbd) {
            workFn(executor::TaskExecutor::CallbackArgs(
                cbd.executor, cbd.myHandle, Status(ErrorCodes::CallbackCanceled, ""), cbd.opCtx));
        };
        return exec->scheduleWork(wrappedTask);
    });

    bool collectionCreated = false;
    storageInterface->createCollFn = [&collectionCreated](
        OperationContext*, const NamespaceString& nss, const CollectionOptions&) {
        collectionCreated = true;
        return Status::OK();
    };

    // Using a non-zero cursor to ensure that
    // the cloner stops the fetcher from retrieving more results.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(ErrorCodes::NamespaceNotFound, "The collection doesn't exist.");
    }

    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_FALSE(collectionCreated);
}

TEST_F(CollectionClonerTest, BeginCollectionScheduleDbWorkFailed) {
    ASSERT_OK(collectionCloner->startup());

    // Replace scheduleDbWork function so that cloner will fail to schedule DB work after
    // getting index specs.
    collectionCloner->setScheduleDbWorkFn_forTest(
        [](const executor::TaskExecutor::CallbackFn& workFn) {
            return StatusWith<executor::TaskExecutor::CallbackHandle>(ErrorCodes::UnknownError, "");
        });

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollectionCallbackCanceled) {
    ASSERT_OK(collectionCloner->startup());

    // Replace scheduleDbWork function so that the callback runs with a cancelled status.
    auto&& executor = getExecutor();
    collectionCloner->setScheduleDbWorkFn_forTest(
        [&](const executor::TaskExecutor::CallbackFn& workFn) {
            executor::TaskExecutor::CallbackHandle handle(std::make_shared<MockCallbackState>());
            mongo::executor::TaskExecutor::CallbackArgs args{
                &executor,
                handle,
                {ErrorCodes::CallbackCanceled, "Never run, but treat like cancelled."}};
            workFn(args);
            return StatusWith<executor::TaskExecutor::CallbackHandle>(handle);
        });

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollectionFailed) {
    ASSERT_OK(collectionCloner->startup());

    storageInterface->createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs) {
        return Status(ErrorCodes::OperationFailed, "");
    };

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();

    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollection) {
    ASSERT_OK(collectionCloner->startup());

    CollectionMockStats stats;
    CollectionBulkLoaderMock* loader = new CollectionBulkLoaderMock(&stats);
    NamespaceString collNss;
    CollectionOptions collOptions;
    std::vector<BSONObj> collIndexSpecs;
    storageInterface->createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
            collNss = theNss;
            collOptions = theOptions;
            collIndexSpecs = theIndexSpecs;
            return std::unique_ptr<CollectionBulkLoader>(loader);
        };

    // Split listIndexes response into 2 batches: first batch contains idIndexSpec and
    // second batch contains specs
    auto nonIdIndexSpecs = makeSecondaryIndexSpecs(nss);

    // First batch contains the _id_ index spec.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(1, BSON_ARRAY(idIndexSpec)));
    }

    // 'status' should not be modified because cloning is not finished.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    // Second batch contains the other index specs.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(
            0, BSON_ARRAY(nonIdIndexSpecs[0] << nonIdIndexSpecs[1]), "nextBatch"));
    }

    collectionCloner->waitForDbWorker();

    // 'status' will be set if listIndexes fails.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());

    ASSERT_EQUALS(nss.ns(), collNss.ns());
    ASSERT_BSONOBJ_EQ(options.toBSON(), collOptions.toBSON());
    ASSERT_EQUALS(nonIdIndexSpecs.size(), collIndexSpecs.size());
    for (std::vector<BSONObj>::size_type i = 0; i < nonIdIndexSpecs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(nonIdIndexSpecs[i], collIndexSpecs[i]);
    }

    // Cloner is still active because it has to read the documents from the source collection.
    ASSERT_TRUE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, FindFetcherScheduleFailed) {
    ASSERT_OK(collectionCloner->startup());

    // Shut down executor while in beginCollection callback.
    // This will cause the fetcher to fail to schedule the find command.
    CollectionMockStats stats;
    CollectionBulkLoaderMock* loader = new CollectionBulkLoaderMock(&stats);
    bool collectionCreated = false;
    storageInterface->createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
            collectionCreated = true;
            getExecutor().shutdown();
            return std::unique_ptr<CollectionBulkLoader>(loader);
        };

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCreated);

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, FindCommandAfterBeginCollection) {
    ASSERT_OK(collectionCloner->startup());

    CollectionMockStats stats;
    CollectionBulkLoaderMock* loader = new CollectionBulkLoaderMock(&stats);
    bool collectionCreated = false;
    storageInterface->createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
            collectionCreated = true;
            return std::unique_ptr<CollectionBulkLoader>(loader);
        };

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCreated);

    // Fetcher should be scheduled after cloner creates collection.
    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    ASSERT_TRUE(net->hasReadyRequests());
    NetworkOperationIterator noi = net->getNextReadyRequest();
    auto&& noiRequest = noi->getRequest();
    ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
    ASSERT_EQUALS("find", std::string(noiRequest.cmdObj.firstElementFieldName()));
    ASSERT_EQUALS(nss.coll().toString(), noiRequest.cmdObj.firstElement().valuestrsafe());
    ASSERT_TRUE(noiRequest.cmdObj.getField("noCursorTimeout").trueValue());
    ASSERT_FALSE(net->hasReadyRequests());
}

TEST_F(CollectionClonerTest, EstablishCursorCommandFailed) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());
    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                         << ""
                                         << "code"
                                         << ErrorCodes::CursorNotFound));
    }

    ASSERT_EQUALS(ErrorCodes::CursorNotFound, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, CollectionClonerResendsFindCommandOnRetriableError) {
    ASSERT_OK(collectionCloner->startup());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);

    // CollectionCollection sends listIndexes request irrespective of collection size in a
    // successful count response.
    assertRemoteCommandNameEquals("count", net->scheduleSuccessfulResponse(createCountResponse(0)));
    net->runReadyNetworkOperations();

    // CollectionCloner requires a successful listIndexes response in order to send the find request
    // for the documents in the collection.
    assertRemoteCommandNameEquals(
        "listIndexes",
        net->scheduleSuccessfulResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec))));
    net->runReadyNetworkOperations();

    // Respond to the find request with a retriable error.
    assertRemoteCommandNameEquals("find",
                                  net->scheduleErrorResponse(Status(ErrorCodes::HostNotFound, "")));
    net->runReadyNetworkOperations();
    ASSERT_TRUE(collectionCloner->isActive());

    // This check exists to ensure that the command used to establish the cursors is retried,
    // regardless of the command format. Therefore, it shouldn't be necessary to have a separate
    // similar test case for the 'parallelCollectionScan' command.
    auto noi = net->getNextReadyRequest();
    assertRemoteCommandNameEquals("find", noi->getRequest());
    net->blackHole(noi);
}

TEST_F(CollectionClonerTest, EstablishCursorCommandCanceled) {
    ASSERT_OK(collectionCloner->startup());

    ASSERT_TRUE(collectionCloner->isActive());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        scheduleNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());

        net->runReadyNetworkOperations();
    }

    collectionCloner->waitForDbWorker();

    ASSERT_TRUE(collectionCloner->isActive());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        scheduleNetworkResponse(BSON("ok" << 1));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->shutdown();

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        getNet()->logQueues();
        net->runReadyNetworkOperations();
    }

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsScheduleDbWorkFailed) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();

    // Replace scheduleDbWork function so that cloner will fail to schedule DB work after
    // getting documents.
    collectionCloner->setScheduleDbWorkFn_forTest(
        [](const executor::TaskExecutor::CallbackFn& workFn) {
            return StatusWith<executor::TaskExecutor::CallbackHandle>(ErrorCodes::UnknownError, "");
        });

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, emptyArray));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(BSON_ARRAY(doc)));
    }

    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsCallbackCanceled) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();

    // Replace scheduleDbWork function so that the callback runs with a cancelled status.
    auto&& executor = getExecutor();
    collectionCloner->setScheduleDbWorkFn_forTest(
        [&](const executor::TaskExecutor::CallbackFn& workFn) {
            executor::TaskExecutor::CallbackHandle handle(std::make_shared<MockCallbackState>());
            mongo::executor::TaskExecutor::CallbackArgs args{
                &executor,
                handle,
                {ErrorCodes::CallbackCanceled, "Never run, but treat like cancelled."}};
            workFn(args);
            return StatusWith<executor::TaskExecutor::CallbackHandle>(handle);
        });

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, emptyArray));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(BSON_ARRAY(BSON("_id" << 1))));
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsFailed) {
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());
    getNet()->logQueues();

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    ASSERT(_loader != nullptr);
    _loader->insertDocsFn = [](const std::vector<BSONObj>::const_iterator begin,
                               const std::vector<BSONObj>::const_iterator end) {
        return Status(ErrorCodes::OperationFailed, "");
    };

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, emptyArray));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(BSON_ARRAY(BSON("_id" << 1))));
    }

    collectionCloner->join();
    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_EQUALS(0, collectionStats.insertCount);

    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus().code());
}

TEST_F(CollectionClonerTest, InsertDocumentsSingleBatch) {
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, emptyArray));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(BSON_ARRAY(doc)));
    }

    collectionCloner->join();
    // TODO: record the documents during insert and compare them
    //       -- maybe better done using a real storage engine, like ephemeral for test.
    ASSERT_EQUALS(1, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsMultipleBatches) {
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, emptyArray));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(doc)));
    }

    collectionCloner->waitForDbWorker();
    // TODO: record the documents during insert and compare them
    //       -- maybe better done using a real storage engine, like ephemeral for test.
    ASSERT_EQUALS(1, collectionStats.insertCount);

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc2 = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(BSON_ARRAY(doc2)));
    }

    collectionCloner->join();
    // TODO: record the documents during insert and compare them
    //       -- maybe better done using a real storage engine, like ephemeral for test.
    ASSERT_EQUALS(2, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, LastBatchContainsNoDocuments) {
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, emptyArray));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(doc)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(1, collectionStats.insertCount);

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc2 = BSON("_id" << 2);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(doc2), "nextBatch"));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(2, collectionStats.insertCount);

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(emptyArray));
    }

    collectionCloner->join();
    ASSERT_EQUALS(2, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, CollectionClonerTransitionsToCompleteIfShutdownBeforeStartup) {
    collectionCloner->shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, collectionCloner->startup());
}

/**
 * Start cloning.
 * Make it fail while copying collection.
 * Restarting cloning should fail with ErrorCodes::ShutdownInProgress error.
 */
TEST_F(CollectionClonerTest, CollectionClonerCannotBeRestartedAfterPreviousFailure) {
    // First cloning attempt - fails while reading documents from source collection.
    unittest::log() << "Starting first collection cloning attempt";
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, emptyArray));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(BSON("_id" << 1))));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(1, collectionStats.insertCount);

    // Check that the status hasn't changed from the initial value.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(ErrorCodes::OperationFailed,
                               "failed to read remaining documents from source collection");
    }

    collectionCloner->join();
    ASSERT_EQUALS(1, collectionStats.insertCount);

    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
    ASSERT_FALSE(collectionCloner->isActive());

    // Second cloning attempt - run to completion.
    unittest::log() << "Starting second collection cloning attempt - startup() should fail";
    collectionStats = CollectionMockStats();
    setStatus(getDetectableErrorStatus());

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, collectionCloner->startup());
}

bool sharedCallbackStateDestroyed = false;
class SharedCallbackState {
    MONGO_DISALLOW_COPYING(SharedCallbackState);

public:
    SharedCallbackState() {}
    ~SharedCallbackState() {
        sharedCallbackStateDestroyed = true;
    }
};

TEST_F(CollectionClonerTest, CollectionClonerResetsOnCompletionCallbackFunctionAfterCompletion) {
    sharedCallbackStateDestroyed = false;
    auto sharedCallbackData = std::make_shared<SharedCallbackState>();

    Status result = getDetectableErrorStatus();
    collectionCloner =
        stdx::make_unique<CollectionCloner>(&getExecutor(),
                                            dbWorkThreadPool.get(),
                                            target,
                                            nss,
                                            options,
                                            [&result, sharedCallbackData](const Status& status) {
                                                log() << "setting result to " << status;
                                                result = status;
                                            },
                                            storageInterface.get(),
                                            defaultBatchSize,
                                            defaultNumCloningCursors);

    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    sharedCallbackData.reset();
    ASSERT_FALSE(sharedCallbackStateDestroyed);

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        auto request =
            net->scheduleErrorResponse(Status(ErrorCodes::OperationFailed, "count command failed"));
        ASSERT_EQUALS("count", request.cmdObj.firstElement().fieldNameStringData());
        net->runReadyNetworkOperations();
    }

    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, result);
    ASSERT_TRUE(sharedCallbackStateDestroyed);
}

TEST_F(CollectionClonerTest,
       CollectionClonerWaitsForPendingTasksToCompleteBeforeInvokingOnCompletionCallback) {
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        assertRemoteCommandNameEquals("count",
                                      net->scheduleSuccessfulResponse(createCountResponse(0)));
        net->runReadyNetworkOperations();

        assertRemoteCommandNameEquals(
            "listIndexes",
            net->scheduleSuccessfulResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec))));
        net->runReadyNetworkOperations();
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, emptyArray));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    // At this point, the CollectionCloner has sent the find request to establish the cursor.
    // We want to return the first batch of documents for the collection from the network so that
    // the CollectionCloner schedules the first _insertDocuments DB task and the getMore request for
    // the next batch of documents.

    // Store the scheduled CollectionCloner::_insertDocuments task but do not run it yet.
    executor::TaskExecutor::CallbackFn insertDocumentsFn;
    collectionCloner->setScheduleDbWorkFn_forTest(
        [&](const executor::TaskExecutor::CallbackFn& workFn) {
            insertDocumentsFn = workFn;
            executor::TaskExecutor::CallbackHandle handle(std::make_shared<MockCallbackState>());
            return StatusWith<executor::TaskExecutor::CallbackHandle>(handle);
        });
    ASSERT_FALSE(insertDocumentsFn);

    // Return first batch of collection documents from remote server for the getMore request.
    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());

        assertRemoteCommandNameEquals(
            "getMore", net->scheduleSuccessfulResponse(createCursorResponse(1, BSON_ARRAY(doc))));
        net->runReadyNetworkOperations();
    }

    // Confirm that CollectionCloner attempted to schedule _insertDocuments task.
    ASSERT_TRUE(insertDocumentsFn);

    // Return an error for the getMore request for the next batch of collection documents.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());

        assertRemoteCommandNameEquals(
            "getMore",
            net->scheduleErrorResponse(Status(ErrorCodes::OperationFailed, "getMore failed")));
        net->runReadyNetworkOperations();
    }

    // CollectionCloner should still be active because we have not finished processing the
    // insertDocuments task.
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());

    // Run the insertDocuments task. The final status of the CollectionCloner should match the first
    // error passed to the completion guard (ie. from the failed getMore request).
    executor::TaskExecutor::CallbackArgs callbackArgs(
        &getExecutor(), {}, Status(ErrorCodes::CallbackCanceled, ""));
    insertDocumentsFn(callbackArgs);

    // Reset 'insertDocumentsFn' to release last reference count on completion guard.
    insertDocumentsFn = {};

    // No need to call CollectionCloner::join() because we invoked the _insertDocuments callback
    // synchronously.

    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
}

class CollectionClonerUUIDTest : public CollectionClonerTest {
protected:
    // The UUID tests should deal gracefully with renamed collections, so start the cloner with
    // an alternate name.
    const NamespaceString alternateNss{"db", "alternateCollName"};
    void startupWithUUID(int maxNumCloningCursors = 1) {
        collectionCloner.reset();
        options.uuid = UUID::gen();
        collectionCloner = stdx::make_unique<CollectionCloner>(&getExecutor(),
                                                               dbWorkThreadPool.get(),
                                                               target,
                                                               alternateNss,
                                                               options,
                                                               setStatusCallback(),
                                                               storageInterface.get(),
                                                               defaultBatchSize,
                                                               maxNumCloningCursors);

        ASSERT_OK(collectionCloner->startup());
    }

    void testWithMaxNumCloningCursors(int maxNumCloningCursors, StringData cmdName) {
        startupWithUUID(maxNumCloningCursors);

        CollectionOptions actualOptions;
        CollectionMockStats stats;
        CollectionBulkLoaderMock* loader = new CollectionBulkLoaderMock(&stats);
        bool collectionCreated = false;
        storageInterface->createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                          const CollectionOptions& theOptions,
                                                          const BSONObj idIndexSpec,
                                                          const std::vector<BSONObj>& theIndexSpecs)
            -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
                collectionCreated = true;
                actualOptions = theOptions;
                return std::unique_ptr<CollectionBulkLoader>(loader);
            };

        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
            processNetworkResponse(createCountResponse(0));
            processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
        }

        collectionCloner->waitForDbWorker();
        ASSERT_TRUE(collectionCreated);

        // Fetcher should be scheduled after cloner creates collection.
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        ASSERT_TRUE(net->hasReadyRequests());
        NetworkOperationIterator noi = net->getNextReadyRequest();
        ASSERT_FALSE(net->hasReadyRequests());
        auto&& noiRequest = noi->getRequest();
        ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
        ASSERT_BSONOBJ_EQ(actualOptions.toBSON(), options.toBSON());

        ASSERT_EQUALS(cmdName, std::string(noiRequest.cmdObj.firstElementFieldName()));
        ASSERT_EQUALS(cmdName == "find", noiRequest.cmdObj.getField("noCursorTimeout").trueValue());
        auto requestUUID = uassertStatusOK(UUID::parse(noiRequest.cmdObj.firstElement()));
        ASSERT_EQUALS(options.uuid.get(), requestUUID);
    }

    /**
     * Sets up a test for the CollectionCloner that simulates the collection being dropped while
     * copying the documents.
     *
     * The mock network returns 'code' to indicate a collection drop.
     */
    void setUpVerifyCollectionWasDroppedTest(ErrorCodes::Error code) {
        startupWithUUID();

        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
            processNetworkResponse(createCountResponse(0));
            processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
        }
        ASSERT_TRUE(collectionCloner->isActive());

        collectionCloner->waitForDbWorker();
        ASSERT_TRUE(collectionCloner->isActive());
        ASSERT_TRUE(collectionStats.initCalled);

        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
            processNetworkResponse(createCursorResponse(1, BSONArray()));
        }

        collectionCloner->waitForDbWorker();
        ASSERT_TRUE(collectionCloner->isActive());

        // Return error response to getMore command.
        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
            processNetworkResponse(code, "collection dropped while copying documents");
        }
    }

    /**
     * Returns the next ready request.
     * Ensures that the request was sent by the CollectionCloner to check if the collection was
     * dropped while copying documents.
     */
    executor::NetworkInterfaceMock::NetworkOperationIterator getVerifyCollectionDroppedRequest(
        executor::NetworkInterfaceMock* net) {
        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        const auto& request = noi->getRequest();
        const auto& cmdObj = request.cmdObj;
        const auto firstElement = cmdObj.firstElement();
        ASSERT_EQUALS("find"_sd, firstElement.fieldNameStringData());
        ASSERT_EQUALS(*options.uuid, unittest::assertGet(UUID::parse(firstElement)));
        return noi;
    }

    /**
     * Start cloning. While copying collection, simulate a collection drop by having the mock
     * network return code 'collectionDropErrCode'.
     *
     * The CollectionCloner should run a find command on the collection by UUID. Simulate successful
     * find command with a drop-pending namespace in the response.  The CollectionCloner should
     * complete with a successful final status.
     */
    void runCloningSuccessfulWithCollectionDropTest(ErrorCodes::Error collectionDropErrCode) {
        setUpVerifyCollectionWasDroppedTest(collectionDropErrCode);

        // CollectionCloner should send a find command with the collection's UUID.
        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
            auto noi = getVerifyCollectionDroppedRequest(getNet());

            // Return a drop-pending namespace in the find response instead of the original
            // collection name passed to CollectionCloner at construction.
            repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
            auto dpns = nss.makeDropPendingNamespace(dropOpTime);
            scheduleNetworkResponse(noi,
                                    createCursorResponse(0, dpns.ns(), BSONArray(), "firstBatch"));
            finishProcessingNetworkResponse();
        }

        // CollectionCloner treats a in collection state to drop-pending during cloning as a
        // successful clone operation.
        collectionCloner->join();
        ASSERT_OK(getStatus());
        ASSERT_FALSE(collectionCloner->isActive());
    }
};

TEST_F(CollectionClonerUUIDTest, FirstRemoteCommandWithUUID) {
    startupWithUUID();

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
    ASSERT_TRUE(net->hasReadyRequests());
    NetworkOperationIterator noi = net->getNextReadyRequest();
    auto&& noiRequest = noi->getRequest();
    ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
    ASSERT_EQUALS("count", std::string(noiRequest.cmdObj.firstElementFieldName()));
    auto requestUUID = uassertStatusOK(UUID::parse(noiRequest.cmdObj.firstElement()));
    ASSERT_EQUALS(options.uuid.get(), requestUUID);

    ASSERT_FALSE(net->hasReadyRequests());
    ASSERT_TRUE(collectionCloner->isActive());
}

TEST_F(CollectionClonerUUIDTest, BeginCollectionWithUUID) {
    startupWithUUID();

    CollectionMockStats stats;
    CollectionBulkLoaderMock* loader = new CollectionBulkLoaderMock(&stats);
    NamespaceString collNss;
    CollectionOptions collOptions;
    BSONObj collIdIndexSpec;
    std::vector<BSONObj> collSecondaryIndexSpecs;
    storageInterface->createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& nonIdIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
            collNss = theNss;
            collOptions = theOptions;
            collIdIndexSpec = idIndexSpec;
            collSecondaryIndexSpecs = nonIdIndexSpecs;
            return std::unique_ptr<CollectionBulkLoader>(loader);
        };

    // Split listIndexes response into 2 batches: first batch contains idIndexSpec and
    // second batch contains specs. We expect the collection cloner to fix up the collection names
    // (here from 'nss' to 'alternateNss') in the index specs, as the collection with the given UUID
    // may be known with a different name by the sync source due to a rename or two-phase drop.
    auto nonIdIndexSpecsToReturnBySyncSource = makeSecondaryIndexSpecs(nss);

    // First batch contains the _id_ index spec.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(1, BSON_ARRAY(idIndexSpec)));
    }

    // 'status' should not be modified because cloning is not finished.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    // Second batch contains the other index specs.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(
            createListIndexesResponse(0,
                                      BSON_ARRAY(nonIdIndexSpecsToReturnBySyncSource[0]
                                                 << nonIdIndexSpecsToReturnBySyncSource[1]),
                                      "nextBatch"));
    }

    collectionCloner->waitForDbWorker();

    // 'status' will be set if listIndexes fails.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());

    ASSERT_EQUALS(collNss.ns(), alternateNss.ns());
    ASSERT_BSONOBJ_EQ(options.toBSON(), collOptions.toBSON());

    BSONObj expectedIdIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                           << "_id_"
                                           << "ns"
                                           << alternateNss.ns());
    ASSERT_BSONOBJ_EQ(collIdIndexSpec, expectedIdIndexSpec);

    auto expectedNonIdIndexSpecs = makeSecondaryIndexSpecs(alternateNss);
    ASSERT_EQUALS(collSecondaryIndexSpecs.size(), expectedNonIdIndexSpecs.size());

    for (std::vector<BSONObj>::size_type i = 0; i < expectedNonIdIndexSpecs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(collSecondaryIndexSpecs[i], expectedNonIdIndexSpecs[i]);
    }

    // Cloner is still active because it has to read the documents from the source collection.
    ASSERT_TRUE(collectionCloner->isActive());
}

TEST_F(CollectionClonerUUIDTest, SingleCloningCursorWithUUIDUsesFindCommand) {
    // With a single cloning cursor, expect a find command.
    testWithMaxNumCloningCursors(1, "find");
}

TEST_F(CollectionClonerUUIDTest, ThreeCloningCursorsWithUUIDUsesParallelCollectionScanCommand) {
    // With three cloning cursors, expect a parallelCollectionScan command.
    testWithMaxNumCloningCursors(3, "parallelCollectionScan");
}

TEST_F(CollectionClonerUUIDTest,
       CloningIsSuccessfulIfCollectionWasDroppedWithCursorNotFoundWhileCopyingDocuments) {
    runCloningSuccessfulWithCollectionDropTest(ErrorCodes::CursorNotFound);
}


TEST_F(CollectionClonerUUIDTest,
       CloningIsSuccessfulIfCollectionWasDroppedWithOperationFailedWhileCopyingDocuments) {
    runCloningSuccessfulWithCollectionDropTest(ErrorCodes::OperationFailed);
}

TEST_F(CollectionClonerUUIDTest,
       CloningIsSuccessfulIfCollectionWasDroppedWithQueryPlanKilledWhileCopyingDocuments) {
    runCloningSuccessfulWithCollectionDropTest(ErrorCodes::QueryPlanKilled);
}

/**
 * Start cloning. While copying collection, simulate a collection drop by having the ARM return a
 * CursorNotFound error.
 *
 * The CollectionCloner should run a find commnd on the collection by UUID. Shut the
 * CollectionCloner down. The CollectionCloner should return a CursorNotFound final status which is
 * the last error from the ARM.
 */
TEST_F(CollectionClonerUUIDTest,
       ShuttingDownCollectionClonerDuringCollectionDropVerificationReturnsCallbackCanceled) {
    setUpVerifyCollectionWasDroppedTest(ErrorCodes::CursorNotFound);

    // CollectionCloner should send a find command with the collection's UUID.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        auto noi = getVerifyCollectionDroppedRequest(getNet());

        // Ignore the find request.
        guard->blackHole(noi);
    }

    // Shut the CollectionCloner down. This should cancel the _verifyCollectionDropped() request.
    collectionCloner->shutdown();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        guard->runReadyNetworkOperations();
    }

    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

class ParallelCollectionClonerTest : public BaseClonerTest {
public:
    BaseCloner* getCloner() const override;

protected:
    void setUp() override;
    void tearDown() override;
    std::vector<BSONObj> generateDocs(std::size_t numDocs);

    auto setStatusCallback() {
        return [this](const Status& s) { setStatus(s); };
    }

    // A simple arbitrary value to use as the default batch size.
    const int defaultBatchSize = 1024;

    // Running initial sync with a single cursor will default to using the 'find' command until
    // 'parallelCollectionScan' has more complete testing.
    const int defaultNumCloningCursors = 3;

    CollectionOptions options;
    std::unique_ptr<CollectionCloner> collectionCloner;
    CollectionMockStats collectionStats;  // Used by the _loader.
    CollectionBulkLoaderMock* _loader;    // Owned by CollectionCloner.
};

void ParallelCollectionClonerTest::setUp() {
    BaseClonerTest::setUp();
    options = {};
    collectionCloner.reset(nullptr);
    collectionCloner = stdx::make_unique<CollectionCloner>(&getExecutor(),
                                                           dbWorkThreadPool.get(),
                                                           target,
                                                           nss,
                                                           options,
                                                           setStatusCallback(),
                                                           storageInterface.get(),
                                                           defaultBatchSize,
                                                           defaultNumCloningCursors);
    collectionStats = CollectionMockStats();
    storageInterface->createCollectionForBulkFn =
        [this](const NamespaceString& nss,
               const CollectionOptions& options,
               const BSONObj idIndexSpec,
               const std::vector<BSONObj>& nonIdIndexSpecs) {
            _loader = new CollectionBulkLoaderMock(&collectionStats);
            Status initCollectionBulkLoader = _loader->init(nonIdIndexSpecs);
            ASSERT_OK(initCollectionBulkLoader);

            return StatusWith<std::unique_ptr<CollectionBulkLoader>>(
                std::unique_ptr<CollectionBulkLoader>(_loader));
        };
}

void ParallelCollectionClonerTest::tearDown() {
    BaseClonerTest::tearDown();
    // Executor may still invoke collection cloner's callback before shutting down.
    collectionCloner.reset(nullptr);
    options = {};
}

BaseCloner* ParallelCollectionClonerTest::getCloner() const {
    return collectionCloner.get();
}

std::vector<BSONObj> ParallelCollectionClonerTest::generateDocs(std::size_t numDocs) {
    std::vector<BSONObj> docs;
    for (unsigned int i = 0; i < numDocs; i++) {
        docs.push_back(BSON("_id" << i));
    }
    return docs;
}

TEST_F(ParallelCollectionClonerTest, InsertDocumentsSingleBatchWithMultipleCloningCursors) {
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    // A single cursor response is returned because there is only a single document to insert.
    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(
            BSON("cursors" << BSON_ARRAY(createCursorResponse(1, emptyArray)) << "ok" << 1));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    auto exec = &getExecutor();
    std::vector<BSONObj> docs;
    // Record the buffered documents before they are inserted so we can
    // validate them.
    collectionCloner->setScheduleDbWorkFn_forTest(
        [&](const executor::TaskExecutor::CallbackFn& workFn) {
            docs = collectionCloner->getDocumentsToInsert_forTest();
            return exec->scheduleWork(workFn);
        });

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(BSON_ARRAY(doc)));
    }

    collectionCloner->join();

    ASSERT_BSONOBJ_EQ(docs[0], doc);
    ASSERT_EQUALS(1, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(ParallelCollectionClonerTest,
       InsertDocumentsSingleBatchOfMultipleDocumentsWithMultipleCloningCursors) {
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    // A single cursor response is returned because there is only a single batch of documents to
    // insert.
    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(
            BSON("cursors" << BSON_ARRAY(createCursorResponse(1, emptyArray)) << "ok" << 1));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    auto exec = &getExecutor();
    std::vector<BSONObj> docs;
    // Record the buffered documents before they are inserted so we can
    // validate them.
    collectionCloner->setScheduleDbWorkFn_forTest(
        [&](const executor::TaskExecutor::CallbackFn& workFn) {
            docs = collectionCloner->getDocumentsToInsert_forTest();
            return exec->scheduleWork(workFn);
        });

    auto generatedDocs = generateDocs(3);

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(
            BSON_ARRAY(generatedDocs[0] << generatedDocs[1] << generatedDocs[2])));
    }

    collectionCloner->join();

    ASSERT_EQUALS(3U, docs.size());
    for (int i = 0; i < 3; i++) {
        ASSERT_BSONOBJ_EQ(docs[i], generatedDocs[i]);
    }
    ASSERT_EQUALS(3, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(ParallelCollectionClonerTest, InsertDocumentsWithMultipleCursorsOfDifferentNumberOfBatches) {
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("cursors" << BSON_ARRAY(createCursorResponse(1, emptyArray)
                                                            << createCursorResponse(2, emptyArray)
                                                            << createCursorResponse(3, emptyArray))
                                              << "ok"
                                              << 1));
    }
    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    auto exec = &getExecutor();
    std::vector<BSONObj> docs;

    // Record the buffered documents before they are inserted so we can
    // validate them.
    collectionCloner->setScheduleDbWorkFn_forTest(
        [&](const executor::TaskExecutor::CallbackFn& workFn) {
            auto buffered = collectionCloner->getDocumentsToInsert_forTest();
            docs.insert(docs.end(), buffered.begin(), buffered.end());
            return exec->scheduleWork(workFn);
        });

    int numDocs = 9;
    std::vector<BSONObj> generatedDocs = generateDocs(numDocs);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(generatedDocs[0]), "nextBatch"));
        processNetworkResponse(createCursorResponse(2, BSON_ARRAY(generatedDocs[1]), "nextBatch"));
        processNetworkResponse(createCursorResponse(3, BSON_ARRAY(generatedDocs[2]), "nextBatch"));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(3U, docs.size());
    for (int i = 0; i < 3; i++) {
        ASSERT_BSONOBJ_EQ(generatedDocs[i], docs[i]);
    }
    ASSERT_EQUALS(3, collectionStats.insertCount);
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(generatedDocs[3]), "nextBatch"));
        processNetworkResponse(createCursorResponse(2, BSON_ARRAY(generatedDocs[4]), "nextBatch"));
        processNetworkResponse(createFinalCursorResponse(BSON_ARRAY(generatedDocs[5])));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(6U, docs.size());
    for (int i = 3; i < 6; i++) {
        ASSERT_BSONOBJ_EQ(generatedDocs[i], docs[i]);
    }
    ASSERT_EQUALS(6, collectionStats.insertCount);
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(generatedDocs[6]), "nextBatch"));
        processNetworkResponse(createFinalCursorResponse(BSON_ARRAY(generatedDocs[7])));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(8U, docs.size());
    for (int i = 6; i < 8; i++) {
        ASSERT_BSONOBJ_EQ(generatedDocs[i], docs[i]);
    }
    ASSERT_EQUALS(8, collectionStats.insertCount);
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(BSON_ARRAY(generatedDocs[8])));
    }

    collectionCloner->join();
    ASSERT_EQUALS(9U, docs.size());
    ASSERT_BSONOBJ_EQ(generatedDocs[8], docs[8]);
    ASSERT_EQUALS(numDocs, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(ParallelCollectionClonerTest, LastBatchContainsNoDocumentsWithMultipleCursors) {
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("cursors" << BSON_ARRAY(createCursorResponse(1, emptyArray)
                                                            << createCursorResponse(2, emptyArray)
                                                            << createCursorResponse(3, emptyArray))
                                              << "ok"
                                              << 1));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    auto exec = &getExecutor();
    std::vector<BSONObj> docs;
    // Record the buffered documents before they are inserted so we can
    // validate them.
    collectionCloner->setScheduleDbWorkFn_forTest(
        [&](const executor::TaskExecutor::CallbackFn& workFn) {
            auto buffered = collectionCloner->getDocumentsToInsert_forTest();
            docs.insert(docs.end(), buffered.begin(), buffered.end());
            return exec->scheduleWork(workFn);
        });

    int numDocs = 6;
    std::vector<BSONObj> generatedDocs = generateDocs(numDocs);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(generatedDocs[0]), "nextBatch"));
        processNetworkResponse(createCursorResponse(2, BSON_ARRAY(generatedDocs[1]), "nextBatch"));
        processNetworkResponse(createCursorResponse(3, BSON_ARRAY(generatedDocs[2]), "nextBatch"));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(3U, docs.size());
    for (int i = 0; i < 3; i++) {
        ASSERT_BSONOBJ_EQ(generatedDocs[i], docs[i]);
    }
    ASSERT_EQUALS(3, collectionStats.insertCount);

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(generatedDocs[3]), "nextBatch"));
        processNetworkResponse(createCursorResponse(2, BSON_ARRAY(generatedDocs[4]), "nextBatch"));
        processNetworkResponse(createCursorResponse(3, BSON_ARRAY(generatedDocs[5]), "nextBatch"));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(6U, docs.size());
    for (int i = 3; i < 6; i++) {
        ASSERT_BSONOBJ_EQ(generatedDocs[i], docs[i]);
    }
    ASSERT_EQUALS(numDocs, collectionStats.insertCount);

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(emptyArray));
        processNetworkResponse(createFinalCursorResponse(emptyArray));
        processNetworkResponse(createFinalCursorResponse(emptyArray));
    }

    collectionCloner->join();
    ASSERT_EQUALS(6, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(ParallelCollectionClonerTest, InsertDocumentsScheduleDbWorkFailedWithMultipleCursors) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();

    // Replace scheduleDbWork function so that cloner will fail to schedule DB work after
    // getting documents.
    collectionCloner->setScheduleDbWorkFn_forTest(
        [](const executor::TaskExecutor::CallbackFn& workFn) {
            return StatusWith<executor::TaskExecutor::CallbackHandle>(ErrorCodes::UnknownError, "");
        });

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("cursors" << BSON_ARRAY(createCursorResponse(1, emptyArray)
                                                            << createCursorResponse(2, emptyArray)
                                                            << createCursorResponse(3, emptyArray))
                                              << "ok"
                                              << 1));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createFinalCursorResponse(BSON_ARRAY(doc)));
    }

    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(ParallelCollectionClonerTest,
       CollectionClonerWaitsForPendingTasksToCompleteBeforeInvokingOnCompletionCallback) {
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        assertRemoteCommandNameEquals("count",
                                      net->scheduleSuccessfulResponse(createCountResponse(0)));
        net->runReadyNetworkOperations();

        assertRemoteCommandNameEquals(
            "listIndexes",
            net->scheduleSuccessfulResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec))));
        net->runReadyNetworkOperations();
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("cursors" << BSON_ARRAY(createCursorResponse(1, emptyArray)
                                                            << createCursorResponse(2, emptyArray)
                                                            << createCursorResponse(3, emptyArray))
                                              << "ok"
                                              << 1));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    // At this point, the CollectionCloner has sent the find request to establish the cursor.
    // We want to return the first batch of documents for the collection from the network so that
    // the CollectionCloner schedules the first _insertDocuments DB task and the getMore request
    // for the next batch of documents.

    // Store the scheduled CollectionCloner::_insertDocuments task but do not run it yet.
    executor::TaskExecutor::CallbackFn insertDocumentsFn;
    collectionCloner->setScheduleDbWorkFn_forTest(
        [&](const executor::TaskExecutor::CallbackFn& workFn) {
            insertDocumentsFn = workFn;
            executor::TaskExecutor::CallbackHandle handle(std::make_shared<MockCallbackState>());
            return StatusWith<executor::TaskExecutor::CallbackHandle>(handle);
        });
    ASSERT_FALSE(insertDocumentsFn);

    // Return first batch of collection documents from remote server for the getMore request.
    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());

        assertRemoteCommandNameEquals(
            "getMore", net->scheduleSuccessfulResponse(createCursorResponse(1, BSON_ARRAY(doc))));
        net->runReadyNetworkOperations();
    }

    // Confirm that CollectionCloner attempted to schedule _insertDocuments task.
    ASSERT_TRUE(insertDocumentsFn);

    // Return an error for the getMore request for the next batch of collection documents.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());

        assertRemoteCommandNameEquals(
            "getMore",
            net->scheduleErrorResponse(Status(ErrorCodes::OperationFailed, "getMore failed")));
        net->runReadyNetworkOperations();
    }

    // CollectionCloner should still be active because we have not finished processing the
    // insertDocuments task.
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());

    // Run the insertDocuments task. The final status of the CollectionCloner should match the
    // first error passed to the completion guard (ie. from the failed getMore request).
    executor::TaskExecutor::CallbackArgs callbackArgs(
        &getExecutor(), {}, Status(ErrorCodes::CallbackCanceled, ""));
    insertDocumentsFn(callbackArgs);

    // Reset 'insertDocumentsFn' to release last reference count on completion guard.
    insertDocumentsFn = {};

    // No need to call CollectionCloner::join() because we invoked the _insertDocuments callback
    // synchronously.

    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
}

}  // namespace
