
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <set>
#include <string>
#include <vector>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using std::set;
using std::string;
using std::vector;
using unittest::assertGet;

using InsertRetryTest = ShardingTestFixture;
using UpdateRetryTest = ShardingTestFixture;

const NamespaceString kTestNamespace("config.TestColl");
const HostAndPort kTestHosts[] = {
    HostAndPort("TestHost1:12345"), HostAndPort("TestHost2:12345"), HostAndPort("TestHost3:12345")};

TEST_F(InsertRetryTest, RetryOnInterruptedAndNetworkErrorSuccess) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        configTargeter()->setFindHostReturnValue({kTestHosts[1]});
        return Status(ErrorCodes::InterruptedDueToReplStateChange, "Interruption");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        configTargeter()->setFindHostReturnValue({kTestHosts[2]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    expectInserts(kTestNamespace, {objToInsert});

    future.timed_get(kFutureTimeout);
}

TEST_F(InsertRetryTest, RetryOnNetworkErrorFails) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_EQ(ErrorCodes::NetworkTimeout, status.code());
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        configTargeter()->setFindHostReturnValue({kTestHosts[1]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        configTargeter()->setFindHostReturnValue({kTestHosts[2]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[2]);
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterNetworkErrorMatch) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        configTargeter()->setFindHostReturnValue({kTestHosts[1]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        return Status(ErrorCodes::DuplicateKey, "Duplicate key");
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        auto query =
            assertGet(QueryRequest::makeFromFindCommand(kTestNamespace, request.cmdObj, false));
        ASSERT_BSONOBJ_EQ(BSON("_id" << 1), query->getFilter());

        return vector<BSONObj>{objToInsert};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterNetworkErrorNotFound) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_EQ(ErrorCodes::DuplicateKey, status.code());
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        configTargeter()->setFindHostReturnValue({kTestHosts[1]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        return Status(ErrorCodes::DuplicateKey, "Duplicate key");
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        auto query =
            assertGet(QueryRequest::makeFromFindCommand(kTestNamespace, request.cmdObj, false));
        ASSERT_BSONOBJ_EQ(BSON("_id" << 1), query->getFilter());

        return vector<BSONObj>();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterNetworkErrorMismatch) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_EQ(ErrorCodes::DuplicateKey, status.code());
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        configTargeter()->setFindHostReturnValue({kTestHosts[1]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        return Status(ErrorCodes::DuplicateKey, "Duplicate key");
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        auto query =
            assertGet(QueryRequest::makeFromFindCommand(kTestNamespace, request.cmdObj, false));
        ASSERT_BSONOBJ_EQ(BSON("_id" << 1), query->getFilter());

        return vector<BSONObj>{BSON("_id" << 1 << "Value"
                                          << "TestValue has changed")};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterWriteConcernFailureMatch) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto insertOp = InsertOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, insertOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setN(1);

        auto wcError = stdx::make_unique<WriteConcernErrorDetail>();

        WriteConcernResult wcRes;
        wcRes.err = "timeout";
        wcRes.wTimedOut = true;

        wcError->setStatus({ErrorCodes::NetworkTimeout, "Failed to wait for write concern"});
        wcError->setErrInfo(BSON("wtimeout" << true));

        response.setWriteConcernError(wcError.release());

        return response.toBSON();
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        return Status(ErrorCodes::DuplicateKey, "Duplicate key");
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        auto query =
            assertGet(QueryRequest::makeFromFindCommand(kTestNamespace, request.cmdObj, false));
        ASSERT_BSONOBJ_EQ(BSON("_id" << 1), query->getFilter());

        return vector<BSONObj>{objToInsert};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(UpdateRetryTest, Success) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(UpdateRetryTest, NotMasterErrorReturnedPersistently) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_EQUALS(ErrorCodes::NotMaster, status);
    });

    for (int i = 0; i < 3; ++i) {
        onCommand([](const RemoteCommandRequest& request) {
            BSONObjBuilder bb;
            CommandHelpers::appendCommandStatusNoThrow(bb, {ErrorCodes::NotMaster, "not master"});
            return bb.obj();
        });
    }

    future.timed_get(kFutureTimeout);
}

TEST_F(UpdateRetryTest, NotMasterReturnedFromTargeter) {
    configTargeter()->setFindHostReturnValue(Status(ErrorCodes::NotMaster, "not master"));

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_EQUALS(ErrorCodes::NotMaster, status);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(UpdateRetryTest, NotMasterOnceSuccessAfterRetry) {
    HostAndPort host1("TestHost1");
    HostAndPort host2("TestHost2");
    configTargeter()->setFindHostReturnValue(host1);

    CollectionType collection;
    collection.setNs(NamespaceString("db.coll"));
    collection.setUpdatedAt(network()->now());
    collection.setUnique(true);
    collection.setEpoch(OID::gen());
    collection.setKeyPattern(KeyPattern(BSON("_id" << 1)));

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        ASSERT_OK(
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern));
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(host1, request.target);

        // Ensure that when the catalog manager tries to retarget after getting the
        // NotMaster response, it will get back a new target.
        configTargeter()->setFindHostReturnValue(host2);

        BSONObjBuilder bb;
        CommandHelpers::appendCommandStatusNoThrow(bb, {ErrorCodes::NotMaster, "not master"});
        return bb.obj();
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(UpdateRetryTest, OperationInterruptedDueToPrimaryStepDown) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());

        auto writeErrDetail = stdx::make_unique<WriteErrorDetail>();
        writeErrDetail->setIndex(0);
        writeErrDetail->setStatus(
            {ErrorCodes::InterruptedDueToReplStateChange, "Operation interrupted"});
        response.addToErrDetails(writeErrDetail.release());

        return response.toBSON();
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(UpdateRetryTest, WriteConcernFailure) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        auto wcError = stdx::make_unique<WriteConcernErrorDetail>();

        WriteConcernResult wcRes;
        wcRes.err = "timeout";
        wcRes.wTimedOut = true;

        wcError->setStatus({ErrorCodes::NetworkTimeout, "Failed to wait for write concern"});
        wcError->setErrInfo(BSON("wtimeout" << true));

        response.setWriteConcernError(wcError.release());

        return response.toBSON();
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
