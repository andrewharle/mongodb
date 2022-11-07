
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

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <thread>
#include <type_traits>

#if defined(__linux__) || defined(__FreeBSD__)
#define MONGO_HAS_SIGALTSTACK 1
#else
#define MONGO_HAS_SIGALTSTACK 0
#endif

namespace mongo {
namespace stdx {
namespace support {

/**
 * Manages an alternate stack for signal handlers.
 * A dummy implementation is provided on platforms which do not support `sigaltstack`.
 */
class SigAltStackController {
public:
#if MONGO_HAS_SIGALTSTACK
    /** Return an object that installs and uninstalls our `_stackStorage` as `sigaltstack`. */
    auto makeInstallGuard() const {
        struct Guard {
            explicit Guard(const SigAltStackController& controller) : _controller(controller) {
                _controller._install();
            }

            ~Guard() {
                _controller._uninstall();
            }

            const SigAltStackController& _controller;
        };
        return Guard{*this};
    }

private:
    void _install() const {
        stack_t ss;
        ss.ss_sp = _stackStorage.get();
        ss.ss_flags = 0;
        ss.ss_size = kStackSize;
        if (sigaltstack(&ss, nullptr)) {
            abort();
        }
    }

    void _uninstall() const {
        stack_t ss;
        ss.ss_flags = SS_DISABLE;
        if (sigaltstack(&ss, nullptr)) {
            abort();
        }
    }

    // Signal stack consumption was measured in mongo/util/stacktrace_test.
    // 64 kiB is 4X our worst case, so that should be enough.
    //   .                                    signal handler action
    //   .  --use-libunwind : ----\       =============================
    //   .  --dbg=on        : -\   \      minimal |  print  | backtrace
    //   .                     =   =      ========|=========|==========
    //   .                     N   N :      4,344 |   7,144 |     5,096
    //   .                     Y   N :      4,424 |   7,528 |     5,160
    //   .                     N   Y :      4,344 |  13,048 |     7,352
    //   .                     Y   Y :      4,424 |  13,672 |     8,392
    //   ( https://jira.mongodb.org/secure/attachment/233569/233569_stacktrace-writeup.txt )
    static constexpr std::size_t kMongoMinSignalStackSize = std::size_t{64} << 10;

    static constexpr std::size_t kStackSize =
        std::max(kMongoMinSignalStackSize, std::size_t{MINSIGSTKSZ});
    std::unique_ptr<char[]> _stackStorage = std::make_unique<char[]>(kStackSize);

#else   // !MONGO_HAS_SIGALTSTACK
    auto makeInstallGuard() const {
        struct Guard {
            ~Guard() {}  // needed to suppress 'unused variable' warnings.
        };
        return Guard{};
    }
#endif  // !MONGO_HAS_SIGALTSTACK
};


template <typename F, typename Tup, size_t... Is>
void apply(F&& f, Tup&& args, std::index_sequence<Is...>) {
    // Use `std::reference_wrapper` to indirectly call `INVOKE` from [func.require],
    // as the `std::thread` constructor would.
    std::ref(f)(std::move(std::get<Is>(args))...);
}

template <typename F, typename Tup>
void apply(F&& f, Tup&& args) {
    apply(std::forward<F>(f),
          std::forward<Tup>(args),
          std::make_index_sequence<std::tuple_size<Tup>::value>{});
}

}  // namespace support

/**
 * We're wrapping std::thread here, rather than aliasing it, because we'd like
 * a std::thread that's identical in all ways to the original, but terminates
 * if a new thread cannot be allocated.  We'd like this behavior because we
 * rarely if ever try/catch thread creation, and don't have a strategy for
 * retrying.  Therefore, all throwing does is remove context as to which part
 * of the system failed thread creation (as the exception itself is caught at
 * the top of the stack).
 *
 * We also want to allocate and install a `sigaltstack` to diagnose stack overflows.
 *
 * We're putting this in stdx, rather than having it as some kind of
 * mongo::Thread, because the signature and use of the type is otherwise
 * completely identical.  Rather than migrate all callers, it was deemed
 * simpler to make the in place adjustment and retain it in stdx.
 *
 * We implement this with private inheritance to minimize the overhead of our
 * wrapping and to simplify the implementation.
 */
class thread : private ::std::thread {  // NOLINT
public:
    using ::std::thread::native_handle_type;  // NOLINT
    using ::std::thread::id;                  // NOLINT

    thread() noexcept : ::std::thread::thread() {}  // NOLINT

    thread(const thread&) = delete;

    thread(thread&& other) noexcept
        : ::std::thread::thread(static_cast<::std::thread&&>(std::move(other))) {}  // NOLINT

    /**
     * As of C++14, the Function overload for std::thread requires that this constructor only
     * participate in overload resolution if std::decay_t<Function> is not the same type as thread.
     * That prevents this overload from intercepting calls that might generate implicit conversions
     * before binding to other constructors (specifically move/copy constructors).
     */
    template <
        class Function,
        class... Args,
        typename std::enable_if<!std::is_same<thread, typename std::decay<Function>::type>::value,
                                int>::type = 0>
    explicit thread(Function f, Args&&... args) try:
        ::std::thread::thread(  // NOLINT
            [
              sigAltStackController = support::SigAltStackController(),
              f = std::move(f),
              pack = std::make_tuple(std::forward<Args>(args)...)
            ]() mutable noexcept {
                auto sigAltStackGuard = sigAltStackController.makeInstallGuard();
                return support::apply(std::move(f), std::move(pack));
            }) {}
    catch (...) {
        std::terminate();
    }

    thread& operator=(const thread&) = delete;

    thread& operator=(thread&& other) noexcept {
        return static_cast<thread&>(
            ::std::thread::operator=(static_cast<::std::thread&&>(std::move(other))));  // NOLINT
    };

    using ::std::thread::joinable;              // NOLINT
    using ::std::thread::get_id;                // NOLINT
    using ::std::thread::native_handle;         // NOLINT
    using ::std::thread::hardware_concurrency;  // NOLINT

    using ::std::thread::join;    // NOLINT
    using ::std::thread::detach;  // NOLINT

    void swap(thread& other) noexcept {
        ::std::thread::swap(static_cast<::std::thread&>(other));  // NOLINT
    }
};

inline void swap(thread& lhs, thread& rhs) noexcept {
    lhs.swap(rhs);
}

namespace this_thread {
using std::this_thread::get_id;  // NOLINT
using std::this_thread::yield;   // NOLINT

#ifdef _WIN32
using std::this_thread::sleep_for;    // NOLINT
using std::this_thread::sleep_until;  // NOLINT
#else
template <class Rep, class Period>
inline void sleep_for(const std::chrono::duration<Rep, Period>& sleep_duration) {  // NOLINT
    if (sleep_duration <= sleep_duration.zero())
        return;

    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(sleep_duration);  // NOLINT
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(sleep_duration - seconds);  // NOLINT
    struct timespec sleepVal = {static_cast<std::time_t>(seconds.count()),
                                static_cast<long>(nanoseconds.count())};
    struct timespec remainVal;
    while (nanosleep(&sleepVal, &remainVal) == -1 && errno == EINTR) {
        sleepVal = remainVal;
    }
}

template <class Clock, class Duration>
void sleep_until(const std::chrono::time_point<Clock, Duration>& sleep_time) {  // NOLINT
    const auto now = Clock::now();
    sleep_for(sleep_time - now);
}
#endif
}  // namespace this_thread

}  // namespace stdx
}  // namespace mongo
