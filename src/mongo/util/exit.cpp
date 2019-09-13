
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl;

#include "mongo/platform/basic.h"

#include "mongo/util/exit.h"

#include <boost/optional.hpp>
#include <functional>
#include <stack>

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/log.h"
#include "mongo/util/quick_exit.h"

namespace mongo {

namespace {

stdx::mutex shutdownMutex;
stdx::condition_variable shutdownTasksComplete;
boost::optional<ExitCode> shutdownExitCode;
bool shutdownTasksInProgress = false;
AtomicUInt32 shutdownFlag;
std::stack<std::function<void(const ShutdownTaskArgs&)>> shutdownTasks;
stdx::thread::id shutdownTasksThreadId;

void runTasks(decltype(shutdownTasks) tasks, const ShutdownTaskArgs& shutdownArgs) {
    while (!tasks.empty()) {
        const auto& task = tasks.top();
        try {
            task(shutdownArgs);
        } catch (...) {
            std::terminate();
        }
        tasks.pop();
    }
}

// The logAndQuickExit_inlock() function should be called while holding the 'shutdownMutex' to
// prevent multiple threads from attempting to log that they are exiting. The quickExit() function
// has its own 'quickExitMutex' to prohibit multiple threads from attempting to call _exit().
MONGO_COMPILER_NORETURN void logAndQuickExit_inlock() {
    ExitCode code = shutdownExitCode.get();
    log() << "shutting down with code:" << code;
    quickExit(code);
}

void setShutdownFlag() {
    shutdownFlag.fetchAndAdd(1);
}

}  // namespace

bool globalInShutdownDeprecated() {
    return shutdownFlag.loadRelaxed() != 0;
}

ExitCode waitForShutdown() {
    stdx::unique_lock<stdx::mutex> lk(shutdownMutex);
    shutdownTasksComplete.wait(lk, [] {
        const auto shutdownStarted = static_cast<bool>(shutdownExitCode);
        return shutdownStarted && !shutdownTasksInProgress;
    });

    return shutdownExitCode.get();
}

void registerShutdownTask(std::function<void(const ShutdownTaskArgs&)> task) {
    stdx::lock_guard<stdx::mutex> lock(shutdownMutex);
    invariant(!globalInShutdownDeprecated());
    shutdownTasks.emplace(std::move(task));
}

void shutdown(ExitCode code, const ShutdownTaskArgs& shutdownArgs) {
    decltype(shutdownTasks) localTasks;

    {
        stdx::unique_lock<stdx::mutex> lock(shutdownMutex);

        if (shutdownTasksInProgress) {
            // Someone better have called shutdown in some form already.
            invariant(globalInShutdownDeprecated());

            // Re-entrant calls to shutdown are not allowed.
            invariant(shutdownTasksThreadId != stdx::this_thread::get_id());

            ExitCode originallyRequestedCode = shutdownExitCode.get();
            if (code != originallyRequestedCode) {
                log() << "While running shutdown tasks with the intent to exit with code "
                      << originallyRequestedCode << ", an additional shutdown request arrived with "
                                                    "the intent to exit with a different exit code "
                      << code << "; ignoring the conflicting exit code";
            }

            // Wait for the shutdown tasks to complete
            while (shutdownTasksInProgress)
                shutdownTasksComplete.wait(lock);

            logAndQuickExit_inlock();
        }

        setShutdownFlag();
        shutdownExitCode.emplace(code);
        shutdownTasksInProgress = true;
        shutdownTasksThreadId = stdx::this_thread::get_id();

        localTasks.swap(shutdownTasks);
    }

    runTasks(std::move(localTasks), shutdownArgs);

    {
        stdx::lock_guard<stdx::mutex> lock(shutdownMutex);
        shutdownTasksInProgress = false;

        shutdownTasksComplete.notify_all();

        logAndQuickExit_inlock();
    }
}

void shutdownNoTerminate(const ShutdownTaskArgs& shutdownArgs) {
    decltype(shutdownTasks) localTasks;

    {
        stdx::lock_guard<stdx::mutex> lock(shutdownMutex);

        if (globalInShutdownDeprecated())
            return;

        setShutdownFlag();
        shutdownTasksInProgress = true;
        shutdownTasksThreadId = stdx::this_thread::get_id();

        localTasks.swap(shutdownTasks);
    }

    runTasks(std::move(localTasks), shutdownArgs);

    {
        stdx::lock_guard<stdx::mutex> lock(shutdownMutex);
        shutdownTasksInProgress = false;
        shutdownExitCode.emplace(EXIT_CLEAN);
    }

    shutdownTasksComplete.notify_all();
}

}  // namespace mongo
