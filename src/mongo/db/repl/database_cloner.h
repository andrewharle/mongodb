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

#include <iosfwd>
#include <list>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class OldThreadPool;

namespace repl {
namespace {

using UniqueLock = stdx::unique_lock<stdx::mutex>;

}  // namespace

class StorageInterface;

class DatabaseCloner : public BaseCloner {
    MONGO_DISALLOW_COPYING(DatabaseCloner);

public:
    struct Stats {
        std::string dbname;
        Date_t start;
        Date_t end;
        size_t collections{0};
        size_t clonedCollections{0};
        std::vector<CollectionCloner::Stats> collectionStats;

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    /**
     * Predicate used on the collection info objects returned by listCollections.
     * Each collection info is represented by a document in the following format:
     * {
     *     name: <collection name>,
     *     options: <collection options>
     * }
     *
     * Returns true if the collection described by the info object should be cloned.
     * Returns false if the collection should be ignored.
     */
    using ListCollectionsPredicateFn = stdx::function<bool(const BSONObj&)>;

    /**
     * Callback function to report progress of collection cloning. Arguments are:
     *     - status from the collection cloner's 'onCompletion' callback.
     *     - source namespace of the collection cloner that completed (or failed).
     *
     * Called exactly once for every collection cloner started by the the database cloner.
     */
    using CollectionCallbackFn = stdx::function<void(const Status&, const NamespaceString&)>;

    /**
     * Type of function to start a collection cloner.
     */
    using StartCollectionClonerFn = stdx::function<Status(CollectionCloner&)>;

    /**
     * Creates DatabaseCloner task in inactive state. Use start() to activate cloner.
     *
     * The cloner calls 'onCompletion' when the database cloning has completed or failed.
     *
     * 'onCompletion' will be called exactly once.
     *
     * Takes ownership of the passed StorageInterface object.
     *
     * 'listCollectionsFilter' will be extended to include collections only, filtering out views.
     */
    DatabaseCloner(executor::TaskExecutor* executor,
                   OldThreadPool* dbWorkThreadPool,
                   const HostAndPort& source,
                   const std::string& dbname,
                   const BSONObj& listCollectionsFilter,
                   const ListCollectionsPredicateFn& listCollectionsPredicate,
                   StorageInterface* storageInterface,
                   const CollectionCallbackFn& collectionWork,
                   const CallbackFn& onCompletion);

    virtual ~DatabaseCloner();

    /**
     * Returns collection info objects read from listCollections result and will not include views.
     */
    const std::vector<BSONObj>& getCollectionInfos_forTest() const;

    std::string getDiagnosticString() const override;

    bool isActive() const override;

    Status startup() noexcept override;

    void shutdown() override;

    void join() override;

    DatabaseCloner::Stats getStats() const;

    std::string getDBName() const;

    //
    // Testing only functions below.
    //

    /**
     * Overrides how executor schedules database work.
     *
     * For testing only.
     */
    void setScheduleDbWorkFn_forTest(const CollectionCloner::ScheduleDbWorkFn& scheduleDbWorkFn);

    /**
     * Overrides how executor starts a collection cloner.
     *
     * For testing only
     */
    void setStartCollectionClonerFn(const StartCollectionClonerFn& startCollectionCloner);

    // State transitions:
    // PreStart --> Running --> ShuttingDown --> Complete
    // It is possible to skip intermediate states. For example,
    // Calling shutdown() when the cloner has not started will transition from PreStart directly
    // to Complete.
    // This enum class is made public for testing.
    enum class State { kPreStart, kRunning, kShuttingDown, kComplete };

    /**
     * Returns current database cloner state.
     * For testing only.
     */
    State getState_forTest() const;

private:
    bool _isActive_inlock() const;

    /**
     * Read collection names and options from listCollections result.
     */
    void _listCollectionsCallback(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                                  Fetcher::NextAction* nextAction,
                                  BSONObjBuilder* getMoreBob);

    /**
     * Forwards collection cloner result to client.
     * Starts a new cloner on a different collection.
     */
    void _collectionClonerCallback(const Status& status, const NamespaceString& nss);

    /**
     * Reports completion status.
     * Sets cloner to inactive.
     */
    void _finishCallback(const Status& status);

    /**
     * Calls the above method after unlocking.
     */
    void _finishCallback_inlock(UniqueLock& lk, const Status& status);

    std::string _getDiagnosticString_inlock() const;

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
    mutable stdx::condition_variable _condition;                 // (M)
    executor::TaskExecutor* _executor;                           // (R)
    OldThreadPool* _dbWorkThreadPool;                            // (R)
    const HostAndPort _source;                                   // (R)
    const std::string _dbname;                                   // (R)
    const BSONObj _listCollectionsFilter;                        // (R)
    const ListCollectionsPredicateFn _listCollectionsPredicate;  // (R)
    StorageInterface* _storageInterface;                         // (R)
    CollectionCallbackFn
        _collectionWork;       // (R) Invoked once for every successfully started collection cloner.
    CallbackFn _onCompletion;  // (R) Invoked once when cloning completes or fails.
    Fetcher _listCollectionsFetcher;  // (R) Fetcher instance for running listCollections command.
    // Collection info objects returned from listCollections.
    // Format of each document:
    // {
    //     name: <collection name>,
    //     options: <collection options>
    // }
    // Holds all collection infos from listCollections.
    std::vector<BSONObj> _collectionInfos;                               // (M)
    std::vector<NamespaceString> _collectionNamespaces;                  // (M)
    std::list<CollectionCloner> _collectionCloners;                      // (M)
    std::list<CollectionCloner>::iterator _currentCollectionClonerIter;  // (M)
    std::vector<std::pair<Status, NamespaceString>> _failedNamespaces;   // (M)
    CollectionCloner::ScheduleDbWorkFn
        _scheduleDbWorkFn;  // (RT) Function for scheduling database work using the executor.
    StartCollectionClonerFn _startCollectionCloner;  // (RT)
    Stats _stats;                                    // (M) Stats about what this instance did.

    // Current database cloner state. See comments for State enum class for details.
    State _state = State::kPreStart;  // (M)
};

/**
 * Insertion operator for DatabaseCloner::State. Formats database cloner state for output stream.
 * For testing only.
 */
std::ostream& operator<<(std::ostream& os, const DatabaseCloner::State& state);

}  // namespace repl
}  // namespace mongo
