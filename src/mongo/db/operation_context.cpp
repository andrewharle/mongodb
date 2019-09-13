
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/operation_context.h"

#include "mongo/bson/inline_decls.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/baton.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/system_tick_source.h"

namespace mongo {

namespace {
// Enabling the maxTimeAlwaysTimeOut fail point will cause any query or command run with a
// valid non-zero max time to fail immediately.  Any getmore operation on a cursor already
// created with a valid non-zero max time will also fail immediately.
//
// This fail point cannot be used with the maxTimeNeverTimeOut fail point.
MONGO_FAIL_POINT_DEFINE(maxTimeAlwaysTimeOut);

// Enabling the maxTimeNeverTimeOut fail point will cause the server to never time out any
// query, command, or getmore operation, regardless of whether a max time is set.
//
// This fail point cannot be used with the maxTimeAlwaysTimeOut fail point.
MONGO_FAIL_POINT_DEFINE(maxTimeNeverTimeOut);

// Enabling the checkForInterruptFail fail point will start a game of random chance on the
// connection specified in the fail point data, generating an interrupt with a given fixed
// probability.  Example invocation:
//
// {configureFailPoint: "checkForInterruptFail",
//  mode: "alwaysOn",
//  data: {threadName: "threadName", chance: .01}}
//
// Both data fields must be specified. In the above example, all interrupt points on the thread with
// name 'threadName' will generate a kill on the current operation with probability p(.01),
// including interrupt points of nested operations. "chance" must be a double between 0 and 1,
// inclusive.
MONGO_FAIL_POINT_DEFINE(checkForInterruptFail);

}  // namespace

OperationContext::OperationContext(Client* client, unsigned int opId)
    : _client(client),
      _opId(opId),
      _elapsedTime(client ? client->getServiceContext()->getTickSource()
                          : SystemTickSource::get()) {}

void OperationContext::setDeadlineAndMaxTime(Date_t when,
                                             Microseconds maxTime,
                                             ErrorCodes::Error timeoutError) {
    invariant(!getClient()->isInDirectClient());
    invariant(ErrorCodes::isExceededTimeLimitError(timeoutError));
    uassert(40120, "Illegal attempt to change operation deadline", !hasDeadline());
    _deadline = when;
    _maxTime = maxTime;
    _timeoutError = timeoutError;
}

Microseconds OperationContext::computeMaxTimeFromDeadline(Date_t when) {
    Microseconds maxTime;
    if (when == Date_t::max()) {
        maxTime = Microseconds::max();
    } else {
        maxTime = when - getServiceContext()->getFastClockSource()->now();
        if (maxTime < Microseconds::zero()) {
            maxTime = Microseconds::zero();
        }
    }
    return maxTime;
}

void OperationContext::setDeadlineByDate(Date_t when, ErrorCodes::Error timeoutError) {
    setDeadlineAndMaxTime(when, computeMaxTimeFromDeadline(when), timeoutError);
}

void OperationContext::setDeadlineAfterNowBy(Microseconds maxTime, ErrorCodes::Error timeoutError) {
    Date_t when;
    if (maxTime < Microseconds::zero()) {
        maxTime = Microseconds::zero();
    }
    if (maxTime == Microseconds::max()) {
        when = Date_t::max();
    } else {
        auto clock = getServiceContext()->getFastClockSource();
        when = clock->now();
        if (maxTime > Microseconds::zero()) {
            when += clock->getPrecision() + maxTime;
        }
    }
    setDeadlineAndMaxTime(when, maxTime, timeoutError);
}

bool OperationContext::hasDeadlineExpired() const {
    if (!hasDeadline()) {
        return false;
    }
    if (MONGO_FAIL_POINT(maxTimeNeverTimeOut)) {
        return false;
    }
    if (MONGO_FAIL_POINT(maxTimeAlwaysTimeOut)) {
        return true;
    }

    // TODO: Remove once all OperationContexts are properly connected to Clients and ServiceContexts
    // in tests.
    if (MONGO_unlikely(!getClient() || !getServiceContext())) {
        return false;
    }

    const auto now = getServiceContext()->getFastClockSource()->now();
    return now >= getDeadline();
}

Milliseconds OperationContext::getRemainingMaxTimeMillis() const {
    if (!hasDeadline()) {
        return Milliseconds::max();
    }

    return std::max(Milliseconds{0},
                    getDeadline() - getServiceContext()->getFastClockSource()->now());
}

Microseconds OperationContext::getRemainingMaxTimeMicros() const {
    if (!hasDeadline()) {
        return Microseconds::max();
    }
    return _maxTime - getElapsedTime();
}

void OperationContext::checkForInterrupt() {
    uassertStatusOK(checkForInterruptNoAssert());
}

namespace {

// Helper function for checkForInterrupt fail point.  Decides whether the operation currently
// being run by the given Client meet the (probabilistic) conditions for interruption as
// specified in the fail point info.
bool opShouldFail(Client* client, const BSONObj& failPointInfo) {
    // Only target the client with the specified connection number.
    if (client->desc() != failPointInfo["threadName"].valuestrsafe()) {
        return false;
    }

    // Return true with (approx) probability p = "chance".  Recall: 0 <= chance <= 1.
    double next = client->getPrng().nextCanonicalDouble();
    if (next > failPointInfo["chance"].numberDouble()) {
        return false;
    }
    return true;
}

}  // namespace

Status OperationContext::checkForInterruptNoAssert() {
    // TODO: Remove the MONGO_likely(getClient()) once all operation contexts are constructed with
    // clients.
    if (MONGO_likely(getClient() && getServiceContext()) &&
        getServiceContext()->getKillAllOperations()) {
        return Status(ErrorCodes::InterruptedAtShutdown, "interrupted at shutdown");
    }

    if (hasDeadlineExpired()) {
        markKilled(_timeoutError);
        return Status(_timeoutError, "operation exceeded time limit");
    }

    MONGO_FAIL_POINT_BLOCK(checkForInterruptFail, scopedFailPoint) {
        if (opShouldFail(getClient(), scopedFailPoint.getData())) {
            log() << "set pending kill on op " << getOpID() << ", for checkForInterruptFail";
            markKilled();
        }
    }

    const auto killStatus = getKillStatus();
    if (killStatus != ErrorCodes::OK) {
        return Status(killStatus, "operation was interrupted");
    }

    return Status::OK();
}

void OperationContext::sleepUntil(Date_t deadline) {
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    invariant(!waitForConditionOrInterruptUntil(cv, lk, deadline, [] { return false; }));
}

void OperationContext::sleepFor(Milliseconds duration) {
    stdx::mutex m;
    stdx::condition_variable cv;
    stdx::unique_lock<stdx::mutex> lk(m);
    invariant(!waitForConditionOrInterruptFor(cv, lk, duration, [] { return false; }));
}

void OperationContext::waitForConditionOrInterrupt(stdx::condition_variable& cv,
                                                   stdx::unique_lock<stdx::mutex>& m) {
    uassertStatusOK(waitForConditionOrInterruptNoAssert(cv, m));
}

Status OperationContext::waitForConditionOrInterruptNoAssert(
    stdx::condition_variable& cv, stdx::unique_lock<stdx::mutex>& m) noexcept {
    auto status = waitForConditionOrInterruptNoAssertUntil(cv, m, Date_t::max());
    if (!status.isOK()) {
        return status.getStatus();
    }
    invariant(status.getValue() == stdx::cv_status::no_timeout);
    return status.getStatus();
}

stdx::cv_status OperationContext::waitForConditionOrInterruptUntil(
    stdx::condition_variable& cv, stdx::unique_lock<stdx::mutex>& m, Date_t deadline) {

    return uassertStatusOK(waitForConditionOrInterruptNoAssertUntil(cv, m, deadline));
}

// Theory of operation for waitForConditionOrInterruptNoAssertUntil and markKilled:
//
// An operation indicates to potential killers that it is waiting on a condition variable by setting
// _waitMutex and _waitCV, while holding the lock on its parent Client. It then unlocks its Client,
// unblocking any killers, which are required to have locked the Client before calling markKilled.
//
// When _waitMutex and _waitCV are set, killers must lock _waitMutex before setting the _killCode,
// and must signal _waitCV before releasing _waitMutex. Unfortunately, they must lock _waitMutex
// without holding a lock on Client to avoid a deadlock with callers of
// waitForConditionOrInterruptNoAssertUntil(). So, in the event that _waitMutex is set, the killer
// increments _numKillers, drops the Client lock, acquires _waitMutex and then re-acquires the
// Client lock. We know that the Client, its OperationContext and _waitMutex will remain valid
// during this period because the caller of waitForConditionOrInterruptNoAssertUntil will not return
// while _numKillers > 0 and will not return until it has itself reacquired _waitMutex. Instead,
// that caller will keep waiting on _waitCV until _numKillers drops to 0.
//
// In essence, when _waitMutex is set, _killCode is guarded by _waitMutex and _waitCV, but when
// _waitMutex is not set, it is guarded by the Client spinlock. Changing _waitMutex is itself
// guarded by the Client spinlock and _numKillers.
//
// When _numKillers does drop to 0, the waiter will null out _waitMutex and _waitCV.
//
// This implementation adds a minimum of two spinlock acquire-release pairs to every condition
// variable wait.
StatusWith<stdx::cv_status> OperationContext::waitForConditionOrInterruptNoAssertUntil(
    stdx::condition_variable& cv, stdx::unique_lock<stdx::mutex>& m, Date_t deadline) noexcept {
    invariant(getClient());
    {
        stdx::lock_guard<Client> clientLock(*getClient());
        invariant(!_waitMutex);
        invariant(!_waitCV);
        invariant(0 == _numKillers);

        // This interrupt check must be done while holding the client lock, so as not to race with a
        // concurrent caller of markKilled.
        auto status = checkForInterruptNoAssert();
        if (!status.isOK()) {
            return status;
        }
        _waitMutex = m.mutex();
        _waitCV = &cv;
    }

    // If the maxTimeNeverTimeOut failpoint is set, behave as though the operation's deadline does
    // not exist. Under normal circumstances, if the op has an existing deadline which is sooner
    // than the deadline passed into this method, we replace our deadline with the op's. This means
    // that we expect to time out at the same time as the existing deadline expires. If, when we
    // time out, we find that the op's deadline has not expired (as will always be the case if
    // maxTimeNeverTimeOut is set) then we assume that the incongruity is due to a clock mismatch
    // and return _timeoutError regardless. To prevent this behaviour, only consider the op's
    // deadline in the event that the maxTimeNeverTimeOut failpoint is not set.
    bool opHasDeadline = (hasDeadline() && !MONGO_FAIL_POINT(maxTimeNeverTimeOut));

    if (opHasDeadline) {
        deadline = std::min(deadline, getDeadline());
    }

    const auto waitStatus = [&] {
        if (Date_t::max() == deadline) {
            cv.wait(m);
            return stdx::cv_status::no_timeout;
        }
        return getServiceContext()->getPreciseClockSource()->waitForConditionUntil(cv, m, deadline);
    }();

    // Continue waiting on cv until no other thread is attempting to kill this one.
    cv.wait(m, [this] {
        stdx::lock_guard<Client> clientLock(*getClient());
        if (0 == _numKillers) {
            _waitMutex = nullptr;
            _waitCV = nullptr;
            return true;
        }
        return false;
    });

    auto status = checkForInterruptNoAssert();
    if (!status.isOK()) {
        return status;
    }
    if (opHasDeadline && waitStatus == stdx::cv_status::timeout && deadline == getDeadline()) {
        // It's possible that the system clock used in stdx::condition_variable::wait_until
        // is slightly ahead of the FastClock used in checkForInterrupt. In this case,
        // we treat the operation as though it has exceeded its time limit, just as if the
        // FastClock and system clock had agreed.
        markKilled(_timeoutError);
        return Status(_timeoutError, "operation exceeded time limit");
    }
    return waitStatus;
}

void OperationContext::markKilled(ErrorCodes::Error killCode) {
    invariant(killCode != ErrorCodes::OK);
    stdx::unique_lock<stdx::mutex> lkWaitMutex;
    if (_waitMutex) {
        invariant(++_numKillers > 0);
        getClient()->unlock();
        ON_BLOCK_EXIT([this]() noexcept {
            getClient()->lock();
            invariant(--_numKillers >= 0);
        });
        lkWaitMutex = stdx::unique_lock<stdx::mutex>{*_waitMutex};
    }
    _killCode.compareAndSwap(ErrorCodes::OK, killCode);
    if (lkWaitMutex && _numKillers == 0) {
        invariant(_waitCV);
        _waitCV->notify_all();
    }

    // If we have a baton, we need to wake it up.  The baton itself will check for interruption
    if (_baton) {
        _baton->notify();
    }
}

void OperationContext::setLogicalSessionId(LogicalSessionId lsid) {
    invariant(!_lsid);
    _lsid = std::move(lsid);
}

void OperationContext::setTxnNumber(TxnNumber txnNumber) {
    invariant(_lsid);
    invariant(!_txnNumber);
    _txnNumber = txnNumber;
}

RecoveryUnit* OperationContext::releaseRecoveryUnit() {
    return _recoveryUnit.release();
}

WriteUnitOfWork::RecoveryUnitState OperationContext::setRecoveryUnit(
    RecoveryUnit* unit, WriteUnitOfWork::RecoveryUnitState state) {
    _recoveryUnit.reset(unit);
    WriteUnitOfWork::RecoveryUnitState oldState = _ruState;
    _ruState = state;
    return oldState;
}

void OperationContext::setLockState(std::unique_ptr<Locker> locker) {
    invariant(!_locker);
    invariant(locker);
    _locker = std::move(locker);
}

std::unique_ptr<Locker> OperationContext::swapLockState(std::unique_ptr<Locker> locker) {
    invariant(_locker);
    invariant(locker);
    _locker.swap(locker);
    return locker;
}

Date_t OperationContext::getExpirationDateForWaitForValue(Milliseconds waitFor) {
    return getServiceContext()->getPreciseClockSource()->now() + waitFor;
}

}  // namespace mongo
