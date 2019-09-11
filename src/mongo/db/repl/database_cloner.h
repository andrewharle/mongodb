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

#include <list>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

class DatabaseCloner : public BaseCloner {
    MONGO_DISALLOW_COPYING(DatabaseCloner);

public:
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
     */
    DatabaseCloner(ReplicationExecutor* executor,
                   const HostAndPort& source,
                   const std::string& dbname,
                   const BSONObj& listCollectionsFilter,
                   const ListCollectionsPredicateFn& listCollectionsPredicate,
                   CollectionCloner::StorageInterface* storageInterface,
                   const CollectionCallbackFn& collectionWork,
                   const CallbackFn& onCompletion);

    virtual ~DatabaseCloner();

    /**
     * Returns collection info objects read from listCollections result.
     * This will return an empty vector until we have processed the last
     * batch of results from listCollections.
     */
    const std::vector<BSONObj>& getCollectionInfos() const;

    std::string getDiagnosticString() const override;

    bool isActive() const override;

    Status start() override;

    void cancel() override;

    void wait() override;

    //
    // Testing only functions below.
    //

    /**
     * Overrides how executor schedules database work.
     *
     * For testing only.
     */
    void setScheduleDbWorkFn(const CollectionCloner::ScheduleDbWorkFn& scheduleDbWorkFn);

    /**
     * Overrides how executor starts a collection cloner.
     *
     * For testing only
     */
    void setStartCollectionClonerFn(const StartCollectionClonerFn& startCollectionCloner);

private:
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

    // Not owned by us.
    ReplicationExecutor* _executor;

    HostAndPort _source;
    std::string _dbname;
    BSONObj _listCollectionsFilter;
    ListCollectionsPredicateFn _listCollectionsPredicate;
    CollectionCloner::StorageInterface* _storageInterface;

    // Invoked once for every successfully started collection cloner.
    CollectionCallbackFn _collectionWork;

    // Invoked once when cloning completes or fails.
    CallbackFn _onCompletion;

    // Protects member data of this database cloner.
    mutable stdx::mutex _mutex;

    mutable stdx::condition_variable _condition;

    // _active is true when database cloner is started.
    bool _active;

    // Fetcher instance for running listCollections command.
    Fetcher _listCollectionsFetcher;

    // Collection info objects returned from listCollections.
    // Format of each document:
    // {
    //     name: <collection name>,
    //     options: <collection options>
    // }
    // Holds all collection infos from listCollections.
    std::vector<BSONObj> _collectionInfos;

    std::vector<NamespaceString> _collectionNamespaces;

    std::list<CollectionCloner> _collectionCloners;
    std::list<CollectionCloner>::iterator _currentCollectionClonerIter;

    // Function for scheduling database work using the executor.
    CollectionCloner::ScheduleDbWorkFn _scheduleDbWorkFn;

    StartCollectionClonerFn _startCollectionCloner;
};

}  // namespace repl
}  // namespace mongo
