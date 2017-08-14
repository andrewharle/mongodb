/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include <cstddef>
#include <iosfwd>
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
namespace repl {

MONGO_FP_FORWARD_DECLARE(stopReplProducer);

/**
 * Used to keep track of the optime and hash of the last fetched operation.
 */
using OpTimeWithHash = OpTimeWith<long long>;

/**
 * The oplog fetcher, once started, reads operations from a remote oplog using a tailable cursor.
 *
 * The initial find command is generated from last fetched optime and hash and may contain the
 * current term depending on the replica set config provided.
 *
 * Forwards metadata in each find/getMore response to the data replicator external state.
 *
 * Performs additional validation on first batch of operations returned from the query to ensure we
 * are able to continue from our last known fetched operation.
 *
 * Validates each batch of operations.
 *
 * Pushes operations from each batch of operations onto a buffer using the "enqueueDocumentsFn"
 * function.
 *
 * Issues a getMore command after successfully processing each batch of operations.
 *
 * When there is an error or when it is not possible to issue another getMore request, calls
 * "onShutdownCallbackFn" to signal the end of processing.
 */
class OplogFetcher {
    MONGO_DISALLOW_COPYING(OplogFetcher);

public:
    static Seconds kDefaultProtocolZeroAwaitDataTimeout;

    /**
     * Type of function called by the oplog fetcher on shutdown with
     * the final oplog fetcher status, last optime fetched and last hash fetched.
     *
     * The status will be Status::OK() if we have processed the last batch of operations
     * from the tailable cursor ("bob" is null in the fetcher callback).
     */
    using OnShutdownCallbackFn =
        stdx::function<void(const Status& shutdownStatus, const OpTimeWithHash& lastFetched)>;

    /**
     * Statistics on current batch of operations returned by the fetcher.
     */
    struct DocumentsInfo {
        size_t networkDocumentCount = 0;
        size_t networkDocumentBytes = 0;
        size_t toApplyDocumentCount = 0;
        size_t toApplyDocumentBytes = 0;
        OpTimeWithHash lastDocument = {0, OpTime()};
    };

    /**
     * Type of function that accepts a pair of iterators into a range of operations
     * within the current batch of results and copies the operations into
     * a buffer to be consumed by the next stage of the replication process.
     *
     * Additional information on the operations is provided in a DocumentsInfo
     * struct.
     */
    using EnqueueDocumentsFn = stdx::function<Status(Fetcher::Documents::const_iterator begin,
                                                     Fetcher::Documents::const_iterator end,
                                                     const DocumentsInfo& info)>;

    /**
     * Validates documents in current batch of results returned from tailing the remote oplog.
     * 'first' should be set to true if this set of documents is the first batch returned from the
     * query.
     * On success, returns statistics on operations.
     */
    static StatusWith<DocumentsInfo> validateDocuments(const Fetcher::Documents& documents,
                                                       bool first,
                                                       Timestamp lastTS);

    /**
     * Initializes fetcher with command to tail remote oplog.
     *
     * Throws a UserException if validation fails on any of the provided arguments.
     */
    OplogFetcher(executor::TaskExecutor* executor,
                 OpTimeWithHash lastFetched,
                 HostAndPort source,
                 NamespaceString nss,
                 ReplSetConfig config,
                 std::size_t maxFetcherRestarts,
                 int requiredRBID,
                 bool requireFresherSyncSource,
                 DataReplicatorExternalState* dataReplicatorExternalState,
                 EnqueueDocumentsFn enqueueDocumentsFn,
                 OnShutdownCallbackFn onShutdownCallbackFn);

    virtual ~OplogFetcher();

    std::string toString() const;

    /**
     * Returns true if we have scheduled the fetcher to read the oplog on the sync source.
     */
    bool isActive() const;

    /**
     * Starts fetcher so that we begin tailing the remote oplog on the sync source.
     */
    Status startup();

    /**
     * Cancels both scheduled and active remote command requests.
     * Returns immediately if the Oplog Fetcher is not active.
     * It is fine to call this multiple times.
     */
    void shutdown();

    /**
     * Waits until the oplog fetcher is inactive.
     * It is fine to call this multiple times.
     */
    void join();

    /**
     * Returns optime and hash of the last oplog entry in the most recent oplog query result.
     */
    OpTimeWithHash getLastOpTimeWithHashFetched() const;

    // ================== Test support API ===================

    /**
     * Returns command object sent in first remote command.
     */
    BSONObj getCommandObject_forTest() const;

    /**
     * Returns metadata object sent in remote commands.
     */
    BSONObj getMetadataObject_forTest() const;

    /**
     * Returns timeout for remote commands to complete.
     */
    Milliseconds getRemoteCommandTimeout_forTest() const;

    /**
     * Returns the await data timeout used for the "maxTimeMS" field in getMore command requests.
     */
    Milliseconds getAwaitDataTimeout_forTest() const;

    // State transitions:
    // PreStart --> Running --> ShuttingDown --> Complete
    // It is possible to skip intermediate states. For example,
    // Calling shutdown() when the cloner has not started will transition from PreStart directly
    // to Complete.
    // This enum class is made public for testing.
    enum class State { kPreStart, kRunning, kShuttingDown, kComplete };

    /**
     * Returns current oplog fetcher state.
     * For testing only.
     */
    State getState_forTest() const;

private:
    bool _isActive_inlock() const;

    /**
     * Schedules fetcher and updates counters.
     */
    Status _scheduleFetcher_inlock();

    /**
     * Processes each batch of results from the tailable cursor started by the fetcher on the sync
     * source.
     *
     * Calls "onShutdownCallbackFn" if there is an error or if there are no further results to
     * request from the sync source.
     */
    void _callback(const Fetcher::QueryResponseStatus& result, BSONObjBuilder* getMoreBob);

    /**
     * Notifies caller that the oplog fetcher has completed processing operations from
     * the remote oplog.
     */
    void _finishCallback(Status status);
    void _finishCallback(Status status, OpTimeWithHash opTimeWithHash);

    /**
     * Creates a new instance of the fetcher to tail the remote oplog starting at the given optime.
     */
    std::unique_ptr<Fetcher> _makeFetcher(long long currentTerm, OpTime lastFetchedOpTime);

    /**
     * Returns whether the oplog fetcher is in shutdown.
     */
    bool _isShuttingDown() const;
    bool _isShuttingDown_inlock() const;

    // Protects member data of this OplogFetcher.
    mutable stdx::mutex _mutex;

    mutable stdx::condition_variable _condition;

    executor::TaskExecutor* const _executor;
    const HostAndPort _source;
    const NamespaceString _nss;
    const BSONObj _metadataObject;

    // Maximum number of times to consecutively restart the fetcher on non-cancellation errors.
    const std::size_t _maxFetcherRestarts;

    // Rollback ID that the sync source is required to have after the first batch.
    int _requiredRBID;

    // A boolean indicating whether we should error if the sync source is not ahead of our initial
    // last fetched OpTime on the first batch. Most of the time this should be set to true,
    // but there are certain special cases, namely during initial sync, where it's acceptable for
    // our sync source to have no ops newer than _lastFetched.
    bool _requireFresherSyncSource;

    DataReplicatorExternalState* const _dataReplicatorExternalState;
    const EnqueueDocumentsFn _enqueueDocumentsFn;
    const Milliseconds _awaitDataTimeout;
    OnShutdownCallbackFn _onShutdownCallbackFn;

    // Used to validate start of first batch of results from the remote oplog
    // tailing query and to keep track of the last known operation consumed via
    // "_enqueueDocumentsFn".
    OpTimeWithHash _lastFetched;

    // Current oplog fetcher state. See comments for State enum class for details.
    State _state = State::kPreStart;

    // Fetcher restarts since the last successful oplog query response.
    std::size_t _fetcherRestarts = 0;

    std::unique_ptr<Fetcher> _fetcher;
    std::unique_ptr<Fetcher> _shuttingDownFetcher;
};

/**
 * Insertion operator for OplogFetcher::State. Formats oplog fetcher state for output stream.
 * For testing only.
 */
std::ostream& operator<<(std::ostream& os, const OplogFetcher::State& state);

}  // namespace repl
}  // namespace mongo
