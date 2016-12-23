/**
 *    Copyright 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

class MultiApplierTest : public executor::ThreadPoolExecutorTest {
public:
private:
    executor::ThreadPoolMock::Options makeThreadPoolMockOptions() const override;
    void setUp() override;
    void tearDown() override;
};

executor::ThreadPoolMock::Options MultiApplierTest::makeThreadPoolMockOptions() const {
    executor::ThreadPoolMock::Options options;
    options.onCreateThread = []() { Client::initThread("MultiApplierTest"); };
    return options;
}

void MultiApplierTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();

    launchExecutorThread();
}

void MultiApplierTest::tearDown() {
    executor::ThreadPoolExecutorTest::shutdownExecutorThread();
    executor::ThreadPoolExecutorTest::joinExecutorThread();

    // Local tear down steps here.

    executor::ThreadPoolExecutorTest::tearDown();
}

Status applyOperation(MultiApplier::OperationPtrs*) {
    return Status::OK();
};

TEST_F(MultiApplierTest, InvalidConstruction) {
    const MultiApplier::Operations operations{OplogEntry(BSON("ts" << Timestamp(Seconds(123), 0)))};
    auto multiApply = [](OperationContext*,
                         MultiApplier::Operations,
                         MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        return Status(ErrorCodes::InternalError, "not implemented");
    };
    auto callback = [](const Status&) {};

    // Null executor.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(nullptr, operations, applyOperation, multiApply, callback),
        UserException,
        ErrorCodes::BadValue,
        "null replication executor");

    // Empty list of operations.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(
            &getExecutor(), MultiApplier::Operations(), applyOperation, multiApply, callback),
        UserException,
        ErrorCodes::BadValue,
        "empty list of operations");

    // Last operation missing timestamp field.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(&getExecutor(), {OplogEntry(BSONObj())}, applyOperation, multiApply, callback),
        UserException,
        ErrorCodes::FailedToParse,
        "last operation missing 'ts' field: {}");

    // "ts" field in last operation not a timestamp.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(
            &getExecutor(), {OplogEntry(BSON("ts" << 123))}, applyOperation, multiApply, callback),
        UserException,
        ErrorCodes::TypeMismatch,
        "'ts' in last operation not a timestamp: { ts: 123 }");

    // Invalid apply operation function.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(
            &getExecutor(), operations, MultiApplier::ApplyOperationFn(), multiApply, callback),
        UserException,
        ErrorCodes::BadValue,
        "apply operation function cannot be null");

    // Invalid multiApply operation function.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(
            &getExecutor(), operations, applyOperation, MultiApplier::MultiApplyFn(), callback),
        UserException,
        ErrorCodes::BadValue,
        "multi apply function cannot be null");

    // Invalid callback function.
    ASSERT_THROWS_CODE_AND_WHAT(
        MultiApplier(
            &getExecutor(), operations, applyOperation, multiApply, MultiApplier::CallbackFn()),
        UserException,
        ErrorCodes::BadValue,
        "callback function cannot be null");
}

TEST_F(MultiApplierTest, MultiApplierInvokesCallbackWithCallbackCanceledStatusUponCancellation) {
    const MultiApplier::Operations operations{OplogEntry(BSON("ts" << Timestamp(Seconds(123), 0)))};

    bool multiApplyInvoked = false;
    auto multiApply = [&](OperationContext* txn,
                          MultiApplier::Operations operations,
                          MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        multiApplyInvoked = true;
        return operations.back().getOpTime();
    };

    auto callbackResult = getDetectableErrorStatus();
    auto callback = [&](const Status& result) { callbackResult = result; };

    MultiApplier multiApplier(&getExecutor(), operations, applyOperation, multiApply, callback);
    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Executor cannot run multiApply callback while we are on the network thread.
        ASSERT_OK(multiApplier.startup());
        multiApplier.shutdown();

        net->runReadyNetworkOperations();
    }
    multiApplier.join();

    ASSERT_FALSE(multiApplyInvoked);

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, callbackResult);
}

TEST_F(MultiApplierTest, MultiApplierPassesMultiApplyErrorToCallback) {
    const MultiApplier::Operations operations{OplogEntry(BSON("ts" << Timestamp(Seconds(123), 0)))};

    bool multiApplyInvoked = false;
    Status multiApplyError(ErrorCodes::OperationFailed, "multi apply failed");
    auto multiApply = [&](OperationContext*,
                          MultiApplier::Operations,
                          MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        multiApplyInvoked = true;
        return multiApplyError;
    };

    auto callbackResult = getDetectableErrorStatus();
    auto callback = [&](const Status& result) { callbackResult = result; };

    MultiApplier multiApplier(&getExecutor(), operations, applyOperation, multiApply, callback);
    ASSERT_OK(multiApplier.startup());
    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->runReadyNetworkOperations();
    }
    multiApplier.join();

    ASSERT_TRUE(multiApplyInvoked);

    ASSERT_EQUALS(multiApplyError, callbackResult);
}

TEST_F(MultiApplierTest, MultiApplierCatchesMultiApplyExceptionAndConvertsToCallbackStatus) {
    const MultiApplier::Operations operations{OplogEntry(BSON("ts" << Timestamp(Seconds(123), 0)))};

    bool multiApplyInvoked = false;
    Status multiApplyError(ErrorCodes::OperationFailed, "multi apply failed");
    auto multiApply = [&](OperationContext* txn,
                          MultiApplier::Operations operations,
                          MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        multiApplyInvoked = true;
        uassertStatusOK(multiApplyError);
        return operations.back().getOpTime();
    };

    auto callbackResult = getDetectableErrorStatus();
    auto callback = [&](const Status& result) { callbackResult = result; };

    MultiApplier multiApplier(&getExecutor(), operations, applyOperation, multiApply, callback);
    ASSERT_OK(multiApplier.startup());
    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->runReadyNetworkOperations();
    }
    multiApplier.join();

    ASSERT_TRUE(multiApplyInvoked);

    ASSERT_EQUALS(multiApplyError, callbackResult);
}

TEST_F(
    MultiApplierTest,
    MultiApplierProvidesOperationContextToMultiApplyFunctionButDisposesBeforeInvokingFinishCallback) {
    const MultiApplier::Operations operations{OplogEntry(BSON("ts" << Timestamp(Seconds(123), 0)))};

    OperationContext* multiApplyTxn = nullptr;
    MultiApplier::Operations operationsToApply;
    auto multiApply = [&](OperationContext* txn,
                          MultiApplier::Operations operations,
                          MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
        multiApplyTxn = txn;
        operationsToApply = operations;
        return operationsToApply.back().getOpTime();
    };

    auto callbackResult = getDetectableErrorStatus();
    OperationContext* callbackTxn = nullptr;
    auto callback = [&](const Status& result) {
        callbackResult = result;
        callbackTxn = cc().getOperationContext();
    };

    MultiApplier multiApplier(&getExecutor(), operations, applyOperation, multiApply, callback);
    ASSERT_OK(multiApplier.startup());
    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->runReadyNetworkOperations();
    }
    multiApplier.join();

    ASSERT_TRUE(multiApplyTxn);
    ASSERT_EQUALS(1U, operationsToApply.size());
    ASSERT_BSONOBJ_EQ(operations[0].raw, operationsToApply[0].raw);

    ASSERT_OK(callbackResult);
    ASSERT_FALSE(callbackTxn);
}

}  // namespace
