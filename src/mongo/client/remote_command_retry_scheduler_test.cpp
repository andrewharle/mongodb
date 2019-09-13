
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
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/jsobj.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

namespace {

using namespace mongo;
using ResponseStatus = executor::TaskExecutor::ResponseStatus;

class CallbackResponseSaver;

class RemoteCommandRetrySchedulerTest : public executor::ThreadPoolExecutorTest {
public:
    void start(RemoteCommandRetryScheduler* scheduler);
    void checkCompletionStatus(RemoteCommandRetryScheduler* scheduler,
                               const CallbackResponseSaver& callbackResponseSaver,
                               const ResponseStatus& response);
    void processNetworkResponse(const ResponseStatus& response);
    void runReadyNetworkOperations();

protected:
    void setUp() override;
};

class CallbackResponseSaver {
    MONGO_DISALLOW_COPYING(CallbackResponseSaver);

public:
    CallbackResponseSaver();

    /**
     * Use this for scheduler callback.
     */
    void operator()(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba);

    std::vector<ResponseStatus> getResponses() const;

private:
    std::vector<ResponseStatus> _responses;
};

/**
 * Task executor proxy with fail point for scheduleRemoteCommand().
 */
class TaskExecutorWithFailureInScheduleRemoteCommand : public unittest::TaskExecutorProxy {
public:
    TaskExecutorWithFailureInScheduleRemoteCommand(executor::TaskExecutor* executor)
        : unittest::TaskExecutorProxy(executor) {}
    virtual StatusWith<executor::TaskExecutor::CallbackHandle> scheduleRemoteCommand(
        const executor::RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const transport::BatonHandle& baton = nullptr) override {
        if (scheduleRemoteCommandFailPoint) {
            return Status(ErrorCodes::ShutdownInProgress,
                          "failed to send remote command - shutdown in progress");
        }
        return getExecutor()->scheduleRemoteCommand(request, cb, baton);
    }

    bool scheduleRemoteCommandFailPoint = false;
};

void RemoteCommandRetrySchedulerTest::start(RemoteCommandRetryScheduler* scheduler) {
    ASSERT_FALSE(scheduler->isActive());

    ASSERT_OK(scheduler->startup());
    ASSERT_TRUE(scheduler->isActive());

    // Starting an already active scheduler should fail.
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, scheduler->startup());
    ASSERT_TRUE(scheduler->isActive());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    ASSERT_TRUE(net->hasReadyRequests());
}

void RemoteCommandRetrySchedulerTest::checkCompletionStatus(
    RemoteCommandRetryScheduler* scheduler,
    const CallbackResponseSaver& callbackResponseSaver,
    const ResponseStatus& response) {
    ASSERT_FALSE(scheduler->isActive());
    auto responses = callbackResponseSaver.getResponses();
    ASSERT_EQUALS(1U, responses.size());
    if (response.isOK()) {
        ASSERT_OK(responses.front().status);
        ASSERT_EQUALS(response, responses.front());
    } else {
        ASSERT_EQUALS(response.status, responses.front().status);
    }
}

void RemoteCommandRetrySchedulerTest::processNetworkResponse(const ResponseStatus& response) {
    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    net->scheduleResponse(noi, net->now(), response);
    net->runReadyNetworkOperations();
}

void RemoteCommandRetrySchedulerTest::runReadyNetworkOperations() {
    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    net->runReadyNetworkOperations();
}

void RemoteCommandRetrySchedulerTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();
    launchExecutorThread();
}

CallbackResponseSaver::CallbackResponseSaver() = default;

void CallbackResponseSaver::operator()(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba) {
    _responses.push_back(rcba.response);
}

std::vector<ResponseStatus> CallbackResponseSaver::getResponses() const {
    return _responses;
}

const executor::RemoteCommandRequest request(HostAndPort("h1:12345"),
                                             "db1",
                                             BSON("ping" << 1),
                                             nullptr);

TEST_F(RemoteCommandRetrySchedulerTest, MakeSingleShotRetryPolicy) {
    auto policy = RemoteCommandRetryScheduler::makeNoRetryPolicy();
    ASSERT_TRUE(policy);
    ASSERT_EQUALS(1U, policy->getMaximumAttempts());
    ASSERT_EQUALS(executor::RemoteCommandRequest::kNoTimeout,
                  policy->getMaximumResponseElapsedTotal());
    // Doesn't matter what "shouldRetryOnError()" returns since we won't be retrying the remote
    // command.
    for (int i = 0; i < int(ErrorCodes::MaxError); ++i) {
        auto error = ErrorCodes::Error(i);
        ASSERT_FALSE(policy->shouldRetryOnError(error));
    }
}

TEST_F(RemoteCommandRetrySchedulerTest, MakeRetryPolicy) {
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        5U,
        Milliseconds(100),
        {ErrorCodes::FailedToParse, ErrorCodes::InvalidNamespace, ErrorCodes::InternalError});
    ASSERT_EQUALS(5U, policy->getMaximumAttempts());
    ASSERT_EQUALS(Milliseconds(100), policy->getMaximumResponseElapsedTotal());
    for (int i = 0; i < int(ErrorCodes::MaxError); ++i) {
        auto error = ErrorCodes::Error(i);
        if (error == ErrorCodes::InternalError || error == ErrorCodes::FailedToParse ||
            error == ErrorCodes::InvalidNamespace) {
            ASSERT_TRUE(policy->shouldRetryOnError(error));
            continue;
        }
        ASSERT_FALSE(policy->shouldRetryOnError(error));
    }
}

TEST_F(RemoteCommandRetrySchedulerTest, InvalidConstruction) {
    auto callback = [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {};
    auto makeRetryPolicy = [] { return RemoteCommandRetryScheduler::makeNoRetryPolicy(); };

    // Null executor.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(nullptr, request, callback, makeRetryPolicy()),
        AssertionException,
        ErrorCodes::BadValue,
        "task executor cannot be null");

    // Empty source in remote command request.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            executor::RemoteCommandRequest(HostAndPort(), request.dbname, request.cmdObj, nullptr),
            callback,
            makeRetryPolicy()),
        AssertionException,
        ErrorCodes::BadValue,
        "source in remote command request cannot be empty");

    // Empty source in remote command request.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            executor::RemoteCommandRequest(request.target, "", request.cmdObj, nullptr),
            callback,
            makeRetryPolicy()),
        AssertionException,
        ErrorCodes::BadValue,
        "database name in remote command request cannot be empty");

    // Empty command object in remote command request.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            executor::RemoteCommandRequest(request.target, request.dbname, BSONObj(), nullptr),
            callback,
            makeRetryPolicy()),
        AssertionException,
        ErrorCodes::BadValue,
        "command object in remote command request cannot be empty");

    // Null remote command callback function.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(&getExecutor(),
                                    request,
                                    executor::TaskExecutor::RemoteCommandCallbackFn(),
                                    makeRetryPolicy()),
        AssertionException,
        ErrorCodes::BadValue,
        "remote command callback function cannot be null");

    // Null retry policy.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(&getExecutor(),
                                    request,
                                    callback,
                                    std::unique_ptr<RemoteCommandRetryScheduler::RetryPolicy>()),
        AssertionException,
        ErrorCodes::BadValue,
        "retry policy cannot be null");

    // Policy max attempts should be positive.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            request,
            callback,
            RemoteCommandRetryScheduler::makeRetryPolicy(0, Milliseconds(100), {})),
        AssertionException,
        ErrorCodes::BadValue,
        "policy max attempts cannot be zero");

    // Policy max response elapsed total cannot be negative.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            request,
            callback,
            RemoteCommandRetryScheduler::makeRetryPolicy(1U, Milliseconds(-100), {})),
        AssertionException,
        ErrorCodes::BadValue,
        "policy max response elapsed total cannot be negative");
}

TEST_F(RemoteCommandRetrySchedulerTest, StartupFailsWhenExecutorIsShutDown) {
    auto callback = [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {};
    auto policy = RemoteCommandRetryScheduler::makeNoRetryPolicy();

    RemoteCommandRetryScheduler scheduler(&getExecutor(), request, callback, std::move(policy));
    ASSERT_FALSE(scheduler.isActive());

    getExecutor().shutdown();

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, scheduler.startup());
    ASSERT_FALSE(scheduler.isActive());
}

TEST_F(RemoteCommandRetrySchedulerTest, StartupFailsWhenSchedulerIsShutDown) {
    auto callback = [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {};
    auto policy = RemoteCommandRetryScheduler::makeNoRetryPolicy();

    RemoteCommandRetryScheduler scheduler(&getExecutor(), request, callback, std::move(policy));
    ASSERT_FALSE(scheduler.isActive());

    scheduler.shutdown();

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, scheduler.startup());
    ASSERT_FALSE(scheduler.isActive());
}

TEST_F(RemoteCommandRetrySchedulerTest,
       ShuttingDownExecutorAfterSchedulerStartupInvokesCallbackWithCallbackCanceledError) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        10U, Milliseconds(1), {ErrorCodes::HostNotFound});
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        ASSERT_EQUALS(request, net->getNextReadyRequest()->getRequest());
    }

    getExecutor().shutdown();

    runReadyNetworkOperations();
    checkCompletionStatus(
        &scheduler, callback, {ErrorCodes::CallbackCanceled, "executor shutdown"});
}

TEST_F(RemoteCommandRetrySchedulerTest,
       ShuttingDownSchedulerAfterSchedulerStartupInvokesCallbackWithCallbackCanceledError) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        10U, Milliseconds(1), {ErrorCodes::HostNotFound});
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    scheduler.shutdown();

    runReadyNetworkOperations();
    checkCompletionStatus(
        &scheduler, callback, {ErrorCodes::CallbackCanceled, "scheduler shutdown"});
}

TEST_F(RemoteCommandRetrySchedulerTest, SchedulerInvokesCallbackOnNonRetryableErrorInResponse) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        10U, Milliseconds(1), RemoteCommandRetryScheduler::kNotMasterErrors);
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    // This should match one of the non-retryable error codes in the policy.
    ResponseStatus rs(ErrorCodes::OperationFailed, "injected error", Milliseconds(0));
    processNetworkResponse(rs);
    checkCompletionStatus(&scheduler, callback, rs);

    // Scheduler cannot be restarted once it has run to completion.
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, scheduler.startup());
}

TEST_F(RemoteCommandRetrySchedulerTest, SchedulerInvokesCallbackOnFirstSuccessfulResponse) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        10U, Milliseconds(1), {ErrorCodes::HostNotFound});
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    // Elapsed time in response is ignored on successful responses.
    ResponseStatus response(BSON("ok" << 1 << "x" << 123), BSON("z" << 456), Milliseconds(100));

    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);

    // Scheduler cannot be restarted once it has run to completion.
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, scheduler.startup());
    ASSERT_FALSE(scheduler.isActive());
}

TEST_F(RemoteCommandRetrySchedulerTest, SchedulerIgnoresEmbeddedErrorInSuccessfulResponse) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        10U, Milliseconds(1), {ErrorCodes::HostNotFound});
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    // Scheduler does not parse document in a successful response for embedded errors.
    // This is the case with some commands (e.g. find) which do not always return errors using the
    // wire protocol.
    ResponseStatus response(BSON("ok" << 0 << "code" << int(ErrorCodes::FailedToParse) << "errmsg"
                                      << "injected error"),
                            BSON("z" << 456),
                            Milliseconds(100));

    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);
}

TEST_F(RemoteCommandRetrySchedulerTest,
       SchedulerInvokesCallbackWithErrorFromExecutorIfScheduleRemoteCommandFailsOnRetry) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        3U, executor::RemoteCommandRequest::kNoTimeout, {ErrorCodes::HostNotFound});
    TaskExecutorWithFailureInScheduleRemoteCommand badExecutor(&getExecutor());
    RemoteCommandRetryScheduler scheduler(
        &badExecutor, request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    processNetworkResponse({ErrorCodes::HostNotFound, "first", Milliseconds(0)});

    // scheduleRemoteCommand() will fail with ErrorCodes::ShutdownInProgress when trying to send
    // third remote command request after processing second failed response.
    badExecutor.scheduleRemoteCommandFailPoint = true;
    processNetworkResponse({ErrorCodes::HostNotFound, "second", Milliseconds(0)});

    checkCompletionStatus(
        &scheduler, callback, {ErrorCodes::ShutdownInProgress, "", Milliseconds(0)});
}

TEST_F(RemoteCommandRetrySchedulerTest,
       SchedulerEnforcesPolicyMaximumAttemptsAndReturnsErrorOfLastFailedRequest) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        3U,
        executor::RemoteCommandRequest::kNoTimeout,
        RemoteCommandRetryScheduler::kAllRetriableErrors);
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    processNetworkResponse({ErrorCodes::HostNotFound, "first", Milliseconds(0)});
    processNetworkResponse({ErrorCodes::HostUnreachable, "second", Milliseconds(0)});

    ResponseStatus response(ErrorCodes::NetworkTimeout, "last", Milliseconds(0));
    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);
}

TEST_F(RemoteCommandRetrySchedulerTest, SchedulerShouldRetryUntilSuccessfulResponseIsReceived) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        3U, executor::RemoteCommandRequest::kNoTimeout, {ErrorCodes::HostNotFound});
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    processNetworkResponse({ErrorCodes::HostNotFound, "first", Milliseconds(0)});

    ResponseStatus response(BSON("ok" << 1 << "x" << 123), BSON("z" << 456), Milliseconds(100));
    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);
}

/**
 * Retry policy that shuts down the scheduler whenever it is consulted by the scheduler.
 * Results from getMaximumAttempts() and shouldRetryOnError() must cause the scheduler
 * to resend the request.
 */
class ShutdownSchedulerRetryPolicy : public RemoteCommandRetryScheduler::RetryPolicy {
public:
    std::size_t getMaximumAttempts() const override {
        if (scheduler) {
            scheduler->shutdown();
        }
        return 2U;
    }
    Milliseconds getMaximumResponseElapsedTotal() const override {
        return executor::RemoteCommandRequest::kNoTimeout;
    }
    bool shouldRetryOnError(ErrorCodes::Error) const override {
        if (scheduler) {
            scheduler->shutdown();
        }
        return true;
    }
    std::string toString() const override {
        return "";
    }

    // This must be set before starting the scheduler.
    RemoteCommandRetryScheduler* scheduler = nullptr;
};

TEST_F(RemoteCommandRetrySchedulerTest,
       SchedulerReturnsCallbackCanceledIfShutdownBeforeSendingRetryCommand) {
    CallbackResponseSaver callback;
    auto policy = stdx::make_unique<ShutdownSchedulerRetryPolicy>();
    auto policyPtr = policy.get();
    TaskExecutorWithFailureInScheduleRemoteCommand badExecutor(&getExecutor());
    RemoteCommandRetryScheduler scheduler(
        &badExecutor, request, stdx::ref(callback), std::move(policy));
    policyPtr->scheduler = &scheduler;
    start(&scheduler);

    processNetworkResponse({ErrorCodes::HostNotFound, "first", Milliseconds(0)});

    checkCompletionStatus(&scheduler,
                          callback,
                          {ErrorCodes::CallbackCanceled,
                           "scheduler was shut down before retrying command",
                           Milliseconds(0)});
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

TEST_F(RemoteCommandRetrySchedulerTest,
       SchedulerResetsOnCompletionCallbackFunctionAfterCompletion) {
    sharedCallbackStateDestroyed = false;
    auto sharedCallbackData = std::make_shared<SharedCallbackState>();

    Status result = getDetectableErrorStatus();
    auto policy = RemoteCommandRetryScheduler::makeNoRetryPolicy();

    RemoteCommandRetryScheduler scheduler(
        &getExecutor(),
        request,
        [&result,
         sharedCallbackData](const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba) {
            unittest::log() << "setting result to " << rcba.response.status;
            result = rcba.response.status;
        },
        std::move(policy));
    start(&scheduler);

    sharedCallbackData.reset();
    ASSERT_FALSE(sharedCallbackStateDestroyed);

    processNetworkResponse({ErrorCodes::OperationFailed, "command failed", Milliseconds(0)});

    scheduler.join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, result);
    ASSERT_TRUE(sharedCallbackStateDestroyed);
}

}  // namespace
