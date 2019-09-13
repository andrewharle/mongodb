
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

#include "mongo/db/periodic_runner_job_abort_expired_transactions.h"

#include "mongo/db/client.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session.h"
#include "mongo/util/log.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

namespace {
const auto gServiceDecoration =
    ServiceContext::declareDecoration<PeriodicThreadToAbortExpiredTransactions>();
}  // anonymous namespace

auto PeriodicThreadToAbortExpiredTransactions::get(ServiceContext* serviceContext)
    -> PeriodicThreadToAbortExpiredTransactions& {
    auto& jobContainer = gServiceDecoration(serviceContext);
    jobContainer._init(serviceContext);

    return jobContainer;
}

auto PeriodicThreadToAbortExpiredTransactions::operator*() const noexcept -> PeriodicJobAnchor& {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return *_anchor;
}

auto PeriodicThreadToAbortExpiredTransactions::operator-> () const noexcept -> PeriodicJobAnchor* {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _anchor.get();
}

void PeriodicThreadToAbortExpiredTransactions::_init(ServiceContext* serviceContext) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_anchor) {
        return;
    }

    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    // We want this job period to be dynamic, to run every (transactionLifetimeLimitSeconds/2)
    // seconds, where transactionLifetimeLimitSeconds is an adjustable server parameter, or within
    // the 1 second to 1 minute range.
    //
    // PeriodicRunner does not currently support altering the period of a job. So we are giving this
    // job a 1 second period on PeriodicRunner and incrementing a static variable 'seconds' on each
    // run until we reach transactionLifetimeLimitSeconds/2, at which point we run the code and
    // reset 'seconds'. Etc.
    PeriodicRunner::PeriodicJob job("startPeriodicThreadToAbortExpiredTransactions",
                                    [](Client* client) {
                                        static int seconds = 0;
                                        int lifetime = transactionLifetimeLimitSeconds.load();

                                        invariant(lifetime >= 1);
                                        int period = lifetime / 2;

                                        // Ensure: 1 <= period <= 60 seconds
                                        period = (period < 1) ? 1 : period;
                                        period = (period > 60) ? 60 : period;

                                        if (++seconds < period) {
                                            return;
                                        }

                                        seconds = 0;

                                        // The opCtx destructor handles unsetting itself from the
                                        // Client. (The PeriodicRunner's Client must be reset before
                                        // returning.)
                                        auto opCtx = client->makeOperationContext();

                                        // Set the Locker such that all lock requests' timeouts will
                                        // be overridden and set to 0. This prevents the expired
                                        // transaction aborter thread from stalling behind any
                                        // non-transaction, exclusive lock taking operation blocked
                                        // behind an active transaction's intent lock.
                                        opCtx->lockState()->setMaxLockTimeout(Milliseconds(0));

                                        killAllExpiredTransactions(opCtx.get());
                                    },
                                    Seconds(1));

    _anchor = std::make_shared<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(job)));
}

}  // namespace mongo
