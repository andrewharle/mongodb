
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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
/**
 * A simple thread-safe fail point implementation that can be activated and
 * deactivated, as well as embed temporary data into it.
 *
 * The fail point has a static instance, which is represented by a FailPoint
 * object, and dynamic instance, which are all the threads in between
 * shouldFailOpenBlock and shouldFailCloseBlock.
 *
 * Sample use:
 * // Declared somewhere:
 * FailPoint makeBadThingsHappen;
 *
 * // Somewhere in the code
 * return false || MONGO_FAIL_POINT(makeBadThingsHappen);
 *
 * or
 *
 * // Somewhere in the code
 * MONGO_FAIL_POINT_BLOCK(makeBadThingsHappen, blockMakeBadThingsHappen) {
 *     const BSONObj& data = blockMakeBadThingsHappen.getData();
 *     // Do something
 * }
 *
 * Invariants:
 *
 * 1. Always refer to _fpInfo first to check if failPoint is active or not before
 *    entering fail point or modifying fail point.
 * 2. Client visible fail point states are read-only when active.
 */
class FailPoint {
    MONGO_DISALLOW_COPYING(FailPoint);

public:
    typedef AtomicUInt32::WordType ValType;
    enum Mode { off, alwaysOn, random, nTimes, skip };
    enum RetCode { fastOff = 0, slowOff, slowOn, userIgnored };

    /**
     * Explicitly resets the seed used for the PRNG in this thread.  If not called on a thread,
     * an instance of SecureRandom is used to seed the PRNG.
     */
    static void setThreadPRNGSeed(int32_t seed);

    /**
     * Parses the FailPoint::Mode, FailPoint::ValType, and data BSONObj from the BSON.
     */
    static StatusWith<std::tuple<Mode, ValType, BSONObj>> parseBSON(const BSONObj& obj);

    FailPoint();

    /**
     * Note: This is not side-effect free - it can change the state to OFF after calling.
     * Note: see MONGO_FAIL_POINT_BLOCK_IF for information on the passed callable
     *
     * @return true if fail point is active.
     */
    template <typename Callable = std::nullptr_t>
    inline bool shouldFail(Callable&& cb = nullptr) {
        RetCode ret = shouldFailOpenBlock(std::forward<Callable>(cb));

        if (MONGO_likely(ret == fastOff)) {
            return false;
        }

        shouldFailCloseBlock();
        return ret == slowOn;
    }

    /**
     * Checks whether fail point is active and increments the reference counter without
     * decrementing it. Must call shouldFailCloseBlock afterwards when the return value
     * is not fastOff. Otherwise, this will remain read-only forever.
     *
     * Note: see MONGO_FAIL_POINT_BLOCK_IF for information on the passed callable
     *
     * @return slowOn if its active and needs to be closed
     *         userIgnored if its active and needs to be closed, but shouldn't be acted on
     *         slowOff if its disabled and needs to be closed
     *         fastOff if its disabled and doesn't need to be closed
     */
    template <typename Callable = std::nullptr_t>
    inline RetCode shouldFailOpenBlock(Callable&& cb = nullptr) {
        if (MONGO_likely((_fpInfo.loadRelaxed() & ACTIVE_BIT) == 0)) {
            return fastOff;
        }

        return slowShouldFailOpenBlock(std::forward<Callable>(cb));
    }

    /**
     * Decrements the reference counter.
     * @see #shouldFailOpenBlock
     */
    void shouldFailCloseBlock();

    /**
     * Changes the settings of this fail point. This will turn off the fail point
     * and waits for all dynamic instances referencing this fail point to go away before
     * actually modifying the settings.
     *
     * @param mode the new mode for this fail point.
     * @param val the value that can have different usage depending on the mode:
     *
     *     - off, alwaysOn: ignored
     *     - random: static_cast<int32_t>(std::numeric_limits<int32_t>::max() * p), where
     *           where p is the probability that any given evaluation of the failpoint should
     *           activate.
     *     - nTimes: the number of times this fail point will be active when
     *         #shouldFail or #shouldFailOpenBlock is called.
     *     - skip: the number of times this failpoint will be inactive when
     *         #shouldFail or #shouldFailOpenBlock is called. After this number is reached, the
     *         failpoint will always be active.
     *
     * @param extra arbitrary BSON object that can be stored to this fail point
     *     that can be referenced afterwards with #getData. Defaults to an empty
     *     document.
     */
    void setMode(Mode mode, ValType val = 0, const BSONObj& extra = BSONObj());

    /**
     * @returns a BSON object showing the current mode and data stored.
     */
    BSONObj toBSON() const;

private:
    static const ValType ACTIVE_BIT = 1 << 31;
    static const ValType REF_COUNTER_MASK = ~ACTIVE_BIT;

    // Bit layout:
    // 31: tells whether this fail point is active.
    // 0~30: unsigned ref counter for active dynamic instances.
    AtomicUInt32 _fpInfo{0};

    // Invariant: These should be read only if ACTIVE_BIT of _fpInfo is set.
    Mode _mode{off};
    AtomicInt32 _timesOrPeriod{0};
    BSONObj _data;

    // protects _mode, _timesOrPeriod, _data
    mutable stdx::mutex _modMutex;

    /**
     * Enables this fail point.
     */
    void enableFailPoint();

    /**
     * Disables this fail point.
     */
    void disableFailPoint();

    /**
     * slow path for #shouldFailOpenBlock
     *
     * If a callable is passed, and returns false, this will return userIgnored and avoid altering
     * the mode in any way.  The argument is the fail point payload.
     */
    RetCode slowShouldFailOpenBlock(stdx::function<bool(const BSONObj&)> cb) noexcept;

    /**
     * @return the stored BSONObj in this fail point. Note that this cannot be safely
     *      read if this fail point is off.
     */
    const BSONObj& getData() const;

    friend class ScopedFailPoint;
};

/**
 * Helper class for making sure that FailPoint#shouldFailCloseBlock is called when
 * FailPoint#shouldFailOpenBlock was called. This should only be used within the
 * MONGO_FAIL_POINT_BLOCK macro.
 */
class ScopedFailPoint {
    MONGO_DISALLOW_COPYING(ScopedFailPoint);

public:
    template <typename Callable = std::nullptr_t>
    ScopedFailPoint(FailPoint* failPoint, Callable&& cb = nullptr) : _failPoint(failPoint) {
        FailPoint::RetCode ret = _failPoint->shouldFailOpenBlock(std::forward<Callable>(cb));
        _shouldClose = ret != FailPoint::fastOff;
        _shouldRun = ret == FailPoint::slowOn;
    }

    ~ScopedFailPoint() {
        if (_shouldClose) {
            _failPoint->shouldFailCloseBlock();
        }
    }

    /**
     * @return true if fail point is on. This will be true at most once.
     */
    inline bool isActive() {
        if (!_shouldRun) {
            return false;
        }

        // We use this in a for loop to prevent iteration, thus flipping to inactive after the first
        // time.
        _shouldRun = false;
        return true;
    }

    /**
     * @return the data stored in the fail point. #isActive must be true
     *     before you can call this.
     */
    const BSONObj& getData() const {
        // Assert when attempting to get data without incrementing ref counter.
        fassert(16445, _shouldClose);
        return _failPoint->getData();
    }

private:
    FailPoint* _failPoint;
    bool _shouldRun;
    bool _shouldClose;
};

#define MONGO_FAIL_POINT(symbol) MONGO_unlikely(symbol.shouldFail())

#define MONGO_FAIL_POINT_PAUSE_WHILE_SET(symbol) \
    do {                                         \
        while (MONGO_FAIL_POINT(symbol)) {       \
            sleepmillis(100);                    \
        }                                        \
    } while (false)

/**
 * Macro for creating a fail point with block context. Also use this when
 * you want to access the data stored in the fail point.
 */
#define MONGO_FAIL_POINT_BLOCK(symbol, blockSymbol) \
    for (mongo::ScopedFailPoint blockSymbol(&symbol); MONGO_unlikely(blockSymbol.isActive());)

/**
 * Macro for creating a fail point with block context and a pre-flight condition. Also use this when
 * you want to access the data stored in the fail point.
 *
 * Your passed in callable should take a const BSONObj& (the fail point payload) and return bool.
 * If it returns true, you'll process the block as normal.  If you return false, you'll exit the
 * block without evaluating it and avoid altering the mode in any way (you won't consume nTimes for
 * instance).
 */
#define MONGO_FAIL_POINT_BLOCK_IF(symbol, blockSymbol, ...)        \
    for (mongo::ScopedFailPoint blockSymbol(&symbol, __VA_ARGS__); \
         MONGO_unlikely(blockSymbol.isActive());)
}  // namespace mongo
