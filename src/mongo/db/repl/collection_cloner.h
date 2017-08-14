/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/callback_completion_guard.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/old_thread_pool.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

class OldThreadPool;

namespace repl {

class StorageInterface;

class CollectionCloner : public BaseCloner {
    MONGO_DISALLOW_COPYING(CollectionCloner);

public:
    /**
     * Callback completion guard for CollectionCloner.
     */
    using OnCompletionGuard = CallbackCompletionGuard<Status>;

    struct Stats {
        static constexpr StringData kDocumentsToCopyFieldName = "documentsToCopy"_sd;
        static constexpr StringData kDocumentsCopiedFieldName = "documentsCopied"_sd;

        std::string ns;
        Date_t start;
        Date_t end;
        size_t documentToCopy{0};
        size_t documentsCopied{0};
        size_t indexes{0};
        size_t fetchBatches{0};

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };
    /**
     * Type of function to schedule storage interface tasks with the executor.
     *
     * Used for testing only.
     */
    using ScheduleDbWorkFn = stdx::function<StatusWith<executor::TaskExecutor::CallbackHandle>(
        const executor::TaskExecutor::CallbackFn&)>;

    /**
     * Creates CollectionCloner task in inactive state. Use start() to activate cloner.
     *
     * The cloner calls 'onCompletion' when the collection cloning has completed or failed.
     *
     * 'onCompletion' will be called exactly once.
     *
     * Takes ownership of the passed StorageInterface object.
     */
    CollectionCloner(executor::TaskExecutor* executor,
                     OldThreadPool* dbWorkThreadPool,
                     const HostAndPort& source,
                     const NamespaceString& sourceNss,
                     const CollectionOptions& options,
                     const CallbackFn& onCompletion,
                     StorageInterface* storageInterface);

    virtual ~CollectionCloner();

    const NamespaceString& getSourceNamespace() const;

    std::string getDiagnosticString() const override;

    bool isActive() const override;

    Status startup() noexcept override;

    void shutdown() override;

    void join() override;

    CollectionCloner::Stats getStats() const;

    //
    // Testing only functions below.
    //

    /**
     * Waits for database worker to complete.
     * Returns immediately if collection cloner is not active.
     *
     * For testing only.
     */
    void waitForDbWorker();

    /**
     * Overrides how executor schedules database work.
     *
     * For testing only.
     */
    void setScheduleDbWorkFn_forTest(const ScheduleDbWorkFn& scheduleDbWorkFn);

private:
    bool _isActive_inlock() const;

    /**
     * Returns whether the CollectionCloner is in shutdown.
     */
    bool _isShuttingDown() const;

    /**
     * Cancels all outstanding work.
     * Used by shutdown() and CompletionGuard::setResultAndCancelRemainingWork().
     */
    void _cancelRemainingWork_inlock();

    /**
     * Read number of documents in collection from count result.
     */
    void _countCallback(const executor::TaskExecutor::RemoteCommandCallbackArgs& args);

    /**
     * Read index specs from listIndexes result.
     */
    void _listIndexesCallback(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                              Fetcher::NextAction* nextAction,
                              BSONObjBuilder* getMoreBob);

    /**
     * Read collection documents from find result.
     */
    void _findCallback(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                       Fetcher::NextAction* nextAction,
                       BSONObjBuilder* getMoreBob,
                       std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Request storage interface to create collection.
     *
     * Called multiple times if there are more than one batch of responses from listIndexes
     * cursor.
     *
     * 'nextAction' is an in/out arg indicating the next action planned and to be taken
     *  by the fetcher.
     */
    void _beginCollectionCallback(const executor::TaskExecutor::CallbackArgs& callbackData);

    /**
     * Called multiple times if there are more than one batch of documents from the fetcher.
     * On the last batch, 'lastBatch' will be true.
     *
     * Each document returned will be inserted via the storage interfaceRequest storage
     * interface.
     */
    void _insertDocumentsCallback(const executor::TaskExecutor::CallbackArgs& callbackData,
                                  bool lastBatch,
                                  std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Reports completion status.
     * Commits/aborts collection building.
     * Sets cloner to inactive.
     */
    void _finishCallback(const Status& status);

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (M)  Reads and writes guarded by _mutex
    // (S)  Self-synchronizing; access in any way from any context.
    // (RT)  Read-only in concurrent operation; synchronized externally by tests
    //
    mutable stdx::mutex _mutex;
    mutable stdx::condition_variable _condition;        // (M)
    executor::TaskExecutor* _executor;                  // (R) Not owned by us.
    OldThreadPool* _dbWorkThreadPool;                   // (R) Not owned by us.
    HostAndPort _source;                                // (R)
    NamespaceString _sourceNss;                         // (R)
    NamespaceString _destNss;                           // (R)
    CollectionOptions _options;                         // (R)
    std::unique_ptr<CollectionBulkLoader> _collLoader;  // (M)
    CallbackFn _onCompletion;             // (M) Invoked once when cloning completes or fails.
    StorageInterface* _storageInterface;  // (R) Not owned by us.
    RemoteCommandRetryScheduler _countScheduler;  // (S)
    Fetcher _listIndexesFetcher;                  // (S)
    std::unique_ptr<Fetcher> _findFetcher;        // (M)
    std::vector<BSONObj> _indexSpecs;             // (M)
    BSONObj _idIndexSpec;                         // (M)
    std::vector<BSONObj> _documents;              // (M) Documents read from fetcher to insert.
    TaskRunner _dbWorkTaskRunner;                 // (R)
    ScheduleDbWorkFn
        _scheduleDbWorkFn;         // (RT) Function for scheduling database work using the executor.
    Stats _stats;                  // (M) stats for this instance.
    ProgressMeter _progressMeter;  // (M) progress meter for this instance.

    // State transitions:
    // PreStart --> Running --> ShuttingDown --> Complete
    // It is possible to skip intermediate states. For example,
    // Calling shutdown() when the cloner has not started will transition from PreStart directly
    // to Complete.
    enum class State { kPreStart, kRunning, kShuttingDown, kComplete };
    State _state = State::kPreStart;  // (M)
};

}  // namespace repl
}  // namespace mongo
