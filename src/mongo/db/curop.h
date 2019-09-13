// @file curop.h


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
#include "mongo/db/commands.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Client;
class CurOp;
class OperationContext;
struct PlanSummaryStats;

/* lifespan is different than CurOp because of recursives with DBDirectClient */
class OpDebug {
public:
    /**
     * Holds counters for execution statistics that are meaningful both for multi-statement
     * transactions and for individual operations outside of a transaction.
     */
    class AdditiveMetrics {
    public:
        /**
         * Adds all the fields of another AdditiveMetrics object together with the fields of this
         * AdditiveMetrics instance.
         */
        void add(const AdditiveMetrics& otherMetrics);

        /**
         * Returns true if the AdditiveMetrics object we are comparing has the same field values as
         * this AdditiveMetrics instance.
         */
        bool equals(const AdditiveMetrics& otherMetrics);

        /**
         * Increments writeConflicts by n.
         */
        void incrementWriteConflicts(long long n);

        /**
         * Increments keysInserted by n.
         */
        void incrementKeysInserted(long long n);

        /**
         * Increments keysDeleted by n.
         */
        void incrementKeysDeleted(long long n);

        /**
         * Increments nmoved by n.
         */
        void incrementNmoved(long long n);

        /**
         * Increments ninserted by n.
         */
        void incrementNinserted(long long n);

        /**
         * Increments prepareReadConflicts by n.
         */
        void incrementPrepareReadConflicts(long long n);

        /**
         * Generates a string showing all non-empty fields. For every non-empty field field1,
         * field2, ..., with corresponding values value1, value2, ..., we will output a string in
         * the format: "<field1>:<value1> <field2>:<value2> ...".
         */
        std::string report();

        boost::optional<long long> keysExamined;
        boost::optional<long long> docsExamined;

        // Number of records that match the query.
        boost::optional<long long> nMatched;
        // Number of records written (no no-ops).
        boost::optional<long long> nModified;
        boost::optional<long long> ninserted;
        boost::optional<long long> ndeleted;

        // Updates resulted in a move (moves are expensive).
        boost::optional<long long> nmoved;
        // Number of index keys inserted.
        boost::optional<long long> keysInserted;
        // Number of index keys removed.
        boost::optional<long long> keysDeleted;
        // Number of read conflicts caused by a prepared transaction.
        boost::optional<long long> prepareReadConflicts;
        boost::optional<long long> writeConflicts;
    };

    OpDebug() = default;

    std::string report(Client* client,
                       const CurOp& curop,
                       const SingleThreadedLockStats* lockStats) const;

    /**
     * Appends information about the current operation to "builder"
     *
     * @param curop reference to the CurOp that owns this OpDebug
     * @param lockStats lockStats object containing locking information about the operation
     */
    void append(const CurOp& curop,
                const SingleThreadedLockStats& lockStats,
                BSONObjBuilder& builder) const;

    /**
     * Copies relevant plan summary metrics to this OpDebug instance.
     */
    void setPlanSummaryMetrics(const PlanSummaryStats& planSummaryStats);

    // -------------------

    // basic options
    // _networkOp represents the network-level op code: OP_QUERY, OP_GET_MORE, OP_COMMAND, etc.
    NetworkOp networkOp{opInvalid};  // only set this through setNetworkOp_inlock() to keep synced
    // _logicalOp is the logical operation type, ie 'dbQuery' regardless of whether this is an
    // OP_QUERY find, a find command using OP_QUERY, or a find command using OP_COMMAND.
    // Similarly, the return value will be dbGetMore for both OP_GET_MORE and getMore command.
    LogicalOp logicalOp{LogicalOp::opInvalid};  // only set this through setNetworkOp_inlock()
    bool iscommand{false};

    // detailed options
    long long cursorid{-1};
    long long ntoreturn{-1};
    long long ntoskip{-1};
    bool exhaust{false};

    bool hasSortStage{false};  // true if the query plan involves an in-memory sort

    // True if the plan came from the multi-planner (not from the plan cache and not a query with a
    // single solution).
    bool fromMultiPlanner{false};

    // True if a replan was triggered during the execution of this operation.
    bool replanned{false};

    bool fastmodinsert{false};  // upsert of an $operation. builds a default object
    bool upsert{false};         // true if the update actually did an insert
    bool cursorExhausted{
        false};  // true if the cursor has been closed at end a find/getMore operation

    BSONObj execStats;  // Owned here.

    // Details of any error (whether from an exception or a command returning failure).
    Status errInfo = Status::OK();

    // response info
    long long executionTimeMicros{0};
    long long nreturned{-1};
    int responseLength{-1};

    // Shard targeting info.
    int nShards{-1};

    // Stores additive metrics.
    AdditiveMetrics additiveMetrics;

    // Stores storage statistics.
    std::shared_ptr<StorageStats> storageStats;
};

/**
 * Container for data used to report information about an OperationContext.
 *
 * Every OperationContext in a server with CurOp support has a stack of CurOp
 * objects. The entry at the top of the stack is used to record timing and
 * resource statistics for the executing operation or suboperation.
 *
 * All of the accessor methods on CurOp may be called by the thread executing
 * the associated OperationContext at any time, or by other threads that have
 * locked the context's owning Client object.
 *
 * The mutator methods on CurOp whose names end _inlock may only be called by the thread
 * executing the associated OperationContext and Client, and only when that thread has also
 * locked the Client object.  All other mutators may only be called by the thread executing
 * CurOp, but do not require holding the Client lock.  The exception to this is the kill()
 * method, which is self-synchronizing.
 *
 * The OpDebug member of a CurOp, accessed via the debug() accessor should *only* be accessed
 * from the thread executing an operation, and as a result its fields may be accessed without
 * any synchronization.
 */
class CurOp {
    MONGO_DISALLOW_COPYING(CurOp);

public:
    static CurOp* get(const OperationContext* opCtx);
    static CurOp* get(const OperationContext& opCtx);

    /**
     * Writes a report of the operation being executed by the given client to the supplied
     * BSONObjBuilder, in a format suitable for display in currentOp. Does not include a lockInfo
     * report, since this may be called in either a mongoD or mongoS context and the latter does not
     * supply lock stats. The client must be locked before calling this method.
     */
    static void reportCurrentOpForClient(OperationContext* opCtx,
                                         Client* client,
                                         bool truncateOps,
                                         BSONObjBuilder* infoBuilder);

    /**
     * Constructs a nested CurOp at the top of the given "opCtx"'s CurOp stack.
     */
    explicit CurOp(OperationContext* opCtx);
    ~CurOp();

    /**
     * Fills out CurOp and OpDebug with basic info common to all commands. We require the NetworkOp
     * in order to distinguish which protocol delivered this request, e.g. OP_QUERY or OP_MSG. This
     * is set early in the request processing backend and does not typically need to be called
     * thereafter. Locks the client as needed to apply the specified settings.
     */
    void setGenericOpRequestDetails(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const Command* command,
                                    BSONObj cmdObj,
                                    NetworkOp op);

    /**
     * Marks the operation end time, records the length of the client response if a valid response
     * exists, and then - subject to the current values of slowMs and sampleRate - logs this CurOp
     * to file under the given LogComponent. Returns 'true' if, in addition to being logged, this
     * operation should also be profiled.
     */
    bool completeAndLogOperation(OperationContext* opCtx,
                                 logger::LogComponent logComponent,
                                 boost::optional<size_t> responseLength = boost::none,
                                 boost::optional<long long> slowMsOverride = boost::none,
                                 bool forceLog = false);

    bool haveOpDescription() const {
        return !_opDescription.isEmpty();
    }

    /**
     * The BSONObj returned may not be owned by CurOp. Callers should call getOwned() if they plan
     * to reference beyond the lifetime of this CurOp instance.
     */
    BSONObj opDescription() const {
        return _opDescription;
    }

    /**
     * Returns an owned BSONObj representing the original command. Used only by the getMore
     * command.
     */
    BSONObj originatingCommand() const {
        return _originatingCommand;
    }

    void enter_inlock(const char* ns, boost::optional<int> dbProfileLevel);

    /**
     * Sets the type of the current network operation.
     */
    void setNetworkOp_inlock(NetworkOp op) {
        _networkOp = op;
        _debug.networkOp = op;
    }

    /**
     * Sets the type of the current logical operation.
     */
    void setLogicalOp_inlock(LogicalOp op) {
        _logicalOp = op;
        _debug.logicalOp = op;
    }

    /**
     * Marks the current operation as being a command.
     */
    void markCommand_inlock() {
        _isCommand = true;
    }

    /**
     * Returns a structure containing data used for profiling, accessed only by a thread
     * currently executing the operation context associated with this CurOp.
     */
    OpDebug& debug() {
        return _debug;
    }

    /**
     * Gets the name of the namespace on which the current operation operates.
     */
    std::string getNS() const {
        return _ns;
    }

    /**
     * Returns true if the elapsed time of this operation is such that it should be profiled or
     * profile level is set to 2. Uses total time if the operation is done, current elapsed time
     * otherwise. The argument shouldSample prevents slow diagnostic logging at profile 1
     * when set to false.
     */
    bool shouldDBProfile(bool shouldSample = true) {
        // Profile level 2 should override any sample rate or slowms settings.
        if (_dbprofile >= 2)
            return true;

        if (!shouldSample || _dbprofile <= 0)
            return false;

        return elapsedTimeExcludingPauses() >= Milliseconds{serverGlobalParams.slowMS};
    }

    /**
     * Raises the profiling level for this operation to "dbProfileLevel" if it was previously
     * less than "dbProfileLevel".
     *
     * This belongs on OpDebug, and so does not have the _inlock suffix.
     */
    void raiseDbProfileLevel(int dbProfileLevel);

    /**
     * Gets the network operation type. No lock is required if called by the thread executing
     * the operation, but the lock must be held if called from another thread.
     */
    NetworkOp getNetworkOp() const {
        return _networkOp;
    }

    /**
     * Gets the logical operation type. No lock is required if called by the thread executing
     * the operation, but the lock must be held if called from another thread.
     */
    LogicalOp getLogicalOp() const {
        return _logicalOp;
    }

    /**
     * Returns true if this CurOp represents a non-command OP_QUERY request.
     */
    bool isLegacyQuery() const {
        return _networkOp == NetworkOp::dbQuery && !isCommand();
    }

    /**
     * Returns true if the current operation is known to be a command.
     */
    bool isCommand() const {
        return _isCommand;
    }

    //
    // Methods for getting/setting elapsed time. Note that the observed elapsed time may be
    // negative, if the system time has been reset during the course of this operation.
    //

    void ensureStarted();
    bool isStarted() const {
        return _start > 0;
    }
    long long startTime() {  // micros
        ensureStarted();
        return _start;
    }
    void done() {
        _end = curTimeMicros64();
    }
    bool isDone() const {
        return _end > 0;
    }

    /**
     * Stops the operation latency timer from "ticking". Time spent paused is not included in the
     * latencies returned by elapsedTimeExcludingPauses().
     *
     * Illegal to call if either the CurOp has not been started, or the CurOp is already in a paused
     * state.
     */
    void pauseTimer() {
        invariant(isStarted());
        invariant(_lastPauseTime == 0);
        _lastPauseTime = curTimeMicros64();
    }

    /**
     * Starts the operation latency timer "ticking" again. Illegal to call if the CurOp has not been
     * started and then subsequently paused.
     */
    void resumeTimer() {
        invariant(isStarted());
        invariant(_lastPauseTime > 0);
        _totalPausedDuration +=
            Microseconds{static_cast<long long>(curTimeMicros64()) - _lastPauseTime};
        _lastPauseTime = 0;
    }

    /**
     * If this op has been marked as done(), returns the wall clock duration between being marked as
     * started with ensureStarted() and the call to done().
     *
     * Otherwise, returns the wall clock duration between the start time and now.
     *
     * If this op has not yet been started, returns 0.
     */
    Microseconds elapsedTimeTotal() {
        if (!isStarted()) {
            return Microseconds{0};
        }

        if (!_end) {
            return Microseconds{static_cast<long long>(curTimeMicros64() - startTime())};
        } else {
            return Microseconds{static_cast<long long>(_end - startTime())};
        }
    }

    /**
     * Returns the total elapsed duration minus any time spent in a paused state. See
     * elapsedTimeTotal() for the definition of the total duration and pause/resumeTimer() for
     * details on pausing.
     *
     * If this op has not yet been started, returns 0.
     *
     * Illegal to call while the timer is paused.
     */
    Microseconds elapsedTimeExcludingPauses() {
        invariant(!_lastPauseTime);
        if (!isStarted()) {
            return Microseconds{0};
        }

        return elapsedTimeTotal() - _totalPausedDuration;
    }

    /**
     * 'opDescription' must be either an owned BSONObj or guaranteed to outlive the OperationContext
     * it is associated with.
     */
    void setOpDescription_inlock(const BSONObj& opDescription) {
        _opDescription = opDescription;
    }

    /**
     * Sets the original command object.
     */
    void setOriginatingCommand_inlock(const BSONObj& commandObj) {
        _originatingCommand = commandObj.getOwned();
    }

    const Command* getCommand() const {
        return _command;
    }
    void setCommand_inlock(const Command* command) {
        _command = command;
    }

    /**
     * Returns whether the current operation is a read, write, or command.
     */
    Command::ReadWriteType getReadWriteType() const;

    /**
     * Appends information about this CurOp to "builder". If "truncateOps" is true, appends a string
     * summary of any objects which exceed the threshold size. If truncateOps is false, append the
     * entire object.
     *
     * If called from a thread other than the one executing the operation associated with this
     * CurOp, it is necessary to lock the associated Client object before executing this method.
     */
    void reportState(BSONObjBuilder* builder, bool truncateOps = false);

    /**
     * Sets the message and the progress meter for this CurOp.
     *
     * While it is necessary to hold the lock while this method executes, the
     * "hit" and "finished" methods of ProgressMeter may be called safely from
     * the thread executing the operation without locking the Client.
     */
    ProgressMeter& setMessage_inlock(const char* msg,
                                     std::string name = "Progress",
                                     unsigned long long progressMeterTotal = 0,
                                     int secondsBetween = 3);

    /**
     * Gets the message for this CurOp.
     */
    const std::string& getMessage() const {
        return _message;
    }
    const ProgressMeter& getProgressMeter() {
        return _progressMeter;
    }
    CurOp* parent() const {
        return _parent;
    }
    void yielded() {
        _numYields++;
    }  // Should be _inlock()?

    /**
     * Returns the number of times yielded() was called.  Callers on threads other
     * than the one executing the operation must lock the client.
     */
    int numYields() const {
        return _numYields;
    }

    /**
     * this should be used very sparingly
     * generally the Context should set this up
     * but sometimes you want to do it ahead of time
     */
    void setNS_inlock(StringData ns);

    StringData getPlanSummary() const {
        return _planSummary;
    }

    void setPlanSummary_inlock(StringData summary) {
        _planSummary = summary.toString();
    }

    void setPlanSummary_inlock(std::string summary) {
        _planSummary = std::move(summary);
    }

    const boost::optional<SingleThreadedLockStats> getLockStatsBase() {
        return _lockStatsBase;
    }

private:
    class CurOpStack;

    static const OperationContext::Decoration<CurOpStack> _curopStack;

    CurOp(OperationContext*, CurOpStack*);

    CurOpStack* _stack;
    CurOp* _parent{nullptr};
    const Command* _command{nullptr};

    // The time at which this CurOp instance was marked as started.
    long long _start{0};

    // The time at which this CurOp instance was marked as done.
    long long _end{0};

    // The time at which this CurOp instance had its timer paused, or 0 if the timer is not
    // currently paused.
    long long _lastPauseTime{0};

    // The cumulative duration for which the timer has been paused.
    Microseconds _totalPausedDuration{0};

    // _networkOp represents the network-level op code: OP_QUERY, OP_GET_MORE, OP_COMMAND, etc.
    NetworkOp _networkOp{opInvalid};  // only set this through setNetworkOp_inlock() to keep synced
    // _logicalOp is the logical operation type, ie 'dbQuery' regardless of whether this is an
    // OP_QUERY find, a find command using OP_QUERY, or a find command using OP_COMMAND.
    // Similarly, the return value will be dbGetMore for both OP_GET_MORE and getMore command.
    LogicalOp _logicalOp{LogicalOp::opInvalid};  // only set this through setNetworkOp_inlock()

    bool _isCommand{false};
    int _dbprofile{0};  // 0=off, 1=slow, 2=all
    std::string _ns;
    BSONObj _opDescription;
    BSONObj _originatingCommand;  // Used by getMore to display original command.
    OpDebug _debug;
    std::string _message;
    ProgressMeter _progressMeter;
    int _numYields{0};

    std::string _planSummary;
    boost::optional<SingleThreadedLockStats>
        _lockStatsBase;  // This is the snapshot of lock stats taken when curOp is constructed.
};

/**
 * Upconverts a legacy query object such that it matches the format of the find command.
 */
BSONObj upconvertQueryEntry(const BSONObj& query,
                            const NamespaceString& nss,
                            int ntoreturn,
                            int ntoskip);

/**
 * Generates a getMore command object from the specified namespace, cursor ID and batchsize.
 */
BSONObj upconvertGetMoreEntry(const NamespaceString& nss, CursorId cursorId, int ntoreturn);

}  // namespace mongo
