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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/databases_cloner.h"

#include <algorithm>
#include <iterator>
#include <set>

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_parameters.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

namespace {

using Request = executor::RemoteCommandRequest;
using Response = executor::RemoteCommandResponse;
using LockGuard = stdx::lock_guard<stdx::mutex>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

// The number of attempts for the listDatabases commands.
MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncListDatabasesAttempts, int, 3);

}  // namespace


DatabasesCloner::DatabasesCloner(StorageInterface* si,
                                 executor::TaskExecutor* exec,
                                 OldThreadPool* dbWorkThreadPool,
                                 HostAndPort source,
                                 IncludeDbFilterFn includeDbPred,
                                 OnFinishFn finishFn)
    : _status(ErrorCodes::NotYetInitialized, ""),
      _exec(exec),
      _dbWorkThreadPool(dbWorkThreadPool),
      _source(source),
      _includeDbFn(includeDbPred),
      _finishFn(finishFn),
      _storage(si) {
    uassert(ErrorCodes::InvalidOptions, "storage interface must be provided.", si);
    uassert(ErrorCodes::InvalidOptions, "executor must be provided.", exec);
    uassert(
        ErrorCodes::InvalidOptions, "db worker thread pool must be provided.", dbWorkThreadPool);
    uassert(ErrorCodes::InvalidOptions, "source must be provided.", !source.empty());
    uassert(ErrorCodes::InvalidOptions, "finishFn must be provided.", finishFn);
    uassert(ErrorCodes::InvalidOptions, "includeDbPred must be provided.", includeDbPred);
};

DatabasesCloner::~DatabasesCloner() {
    DESTRUCTOR_GUARD(shutdown(); join(););
}

std::string DatabasesCloner::toString() const {
    LockGuard lk(_mutex);
    return str::stream() << "initial sync --"
                         << " active:" << _active << " status:" << _status.toString()
                         << " source:" << _source.toString()
                         << " db cloners completed:" << _stats.databasesCloned
                         << " db count:" << _databaseCloners.size();
}

void DatabasesCloner::join() {
    if (auto listDatabaseScheduler = _getListDatabasesScheduler()) {
        listDatabaseScheduler->join();
    }

    auto databaseCloners = _getDatabaseCloners();
    for (auto&& cloner : databaseCloners) {
        cloner->join();
    }
}

void DatabasesCloner::shutdown() {
    if (auto listDatabaseScheduler = _getListDatabasesScheduler()) {
        listDatabaseScheduler->shutdown();
    }

    auto databaseCloners = _getDatabaseCloners();
    for (auto&& cloner : databaseCloners) {
        cloner->shutdown();
    }

    LockGuard lk(_mutex);
    if (!_active) {
        return;
    }
    _active = false;
    _setStatus_inlock({ErrorCodes::CallbackCanceled, "Initial Sync Cancelled."});
}

bool DatabasesCloner::isActive() {
    LockGuard lk(_mutex);
    return _active;
}

Status DatabasesCloner::getStatus() {
    LockGuard lk(_mutex);
    return _status;
}

DatabasesCloner::Stats DatabasesCloner::getStats() const {
    LockGuard lk(_mutex);
    DatabasesCloner::Stats stats = _stats;
    for (auto&& databaseCloner : _databaseCloners) {
        stats.databaseStats.emplace_back(databaseCloner->getStats());
    }
    return stats;
}

std::string DatabasesCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj DatabasesCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    append(&bob);
    return bob.obj();
}

void DatabasesCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber("databasesCloned", databasesCloned);
    for (auto&& db : databaseStats) {
        BSONObjBuilder dbBuilder(builder->subobjStart(db.dbname));
        db.append(&dbBuilder);
        dbBuilder.doneFast();
    }
}

Status DatabasesCloner::startup() {
    UniqueLock lk(_mutex);
    invariant(!_active);
    _active = true;

    if (!_status.isOK() && _status.code() != ErrorCodes::NotYetInitialized) {
        return _status;
    }

    _status = Status::OK();

    // Schedule listDatabase command which will kick off the database cloner per result db.
    Request listDBsReq(_source,
                       "admin",
                       BSON("listDatabases" << true),
                       rpc::ServerSelectionMetadata(true, boost::none).toBSON(),
                       nullptr);
    _listDBsScheduler = stdx::make_unique<RemoteCommandRetryScheduler>(
        _exec,
        listDBsReq,
        stdx::bind(&DatabasesCloner::_onListDatabaseFinish, this, stdx::placeholders::_1),
        RemoteCommandRetryScheduler::makeRetryPolicy(
            numInitialSyncListDatabasesAttempts,
            executor::RemoteCommandRequest::kNoTimeout,
            RemoteCommandRetryScheduler::kAllRetriableErrors));
    auto s = _listDBsScheduler->startup();
    if (!s.isOK()) {
        _fail_inlock(&lk, s);
    }

    return _status;
}

void DatabasesCloner::setScheduleDbWorkFn_forTest(const CollectionCloner::ScheduleDbWorkFn& work) {
    LockGuard lk(_mutex);
    _scheduleDbWorkFn = work;
}

void DatabasesCloner::_onListDatabaseFinish(const CommandCallbackArgs& cbd) {
    Status respStatus = cbd.response.status;
    if (respStatus.isOK()) {
        respStatus = getStatusFromCommandResult(cbd.response.data);
    }

    UniqueLock lk(_mutex);
    if (!respStatus.isOK()) {
        LOG(1) << "listDatabases failed: " << respStatus;
        _fail_inlock(&lk, respStatus);
        return;
    }

    const auto respBSON = cbd.response.data;
    // There should not be any cloners yet
    invariant(_databaseCloners.size() == 0);
    const auto dbsElem = respBSON["databases"].Obj();
    BSONForEach(arrayElement, dbsElem) {
        const BSONObj dbBSON = arrayElement.Obj();

        // Check to see if we want to exclude this db from the clone.
        if (!_includeDbFn(dbBSON)) {
            LOG(1) << "excluding db: " << dbBSON;
            continue;
        }

        const std::string dbName = dbBSON["name"].str();
        std::shared_ptr<DatabaseCloner> dbCloner{nullptr};

        // filters for DatabasesCloner.
        const auto collectionFilterPred = [dbName](const BSONObj& collInfo) {
            const auto collName = collInfo["name"].str();
            const NamespaceString ns(dbName, collName);
            if (ns.isSystem() && !legalClientSystemNS(ns.ns())) {
                LOG(1) << "Skipping 'system' collection: " << ns.ns();
                return false;
            }
            if (!ns.isNormal()) {
                LOG(1) << "Skipping non-normal collection: " << ns.ns();
                return false;
            }

            LOG(2) << "Allowing cloning of collectionInfo: " << collInfo;
            return true;
        };
        const auto onCollectionFinish = [](const Status& status, const NamespaceString& srcNss) {
            if (status.isOK()) {
                LOG(1) << "collection clone finished: " << srcNss;
            } else {
                warning() << "collection clone for '" << srcNss << "' failed due to "
                          << status.toString();
            }
        };
        const auto onDbFinish = [this, dbName](const Status& status) {
            _onEachDBCloneFinish(status, dbName);
        };
        Status startStatus = Status::OK();
        try {
            dbCloner.reset(new DatabaseCloner(
                _exec,
                _dbWorkThreadPool,
                _source,
                dbName,
                BSONObj(),  // do not filter collections out during listCollections call.
                collectionFilterPred,
                _storage,  // use storage provided.
                onCollectionFinish,
                onDbFinish));
            if (_scheduleDbWorkFn) {
                dbCloner->setScheduleDbWorkFn_forTest(_scheduleDbWorkFn);
            }
            // Start first database cloner.
            if (_databaseCloners.empty()) {
                startStatus = dbCloner->startup();
            }
        } catch (...) {
            startStatus = exceptionToStatus();
        }

        if (!startStatus.isOK()) {
            std::string err = str::stream() << "could not create cloner for database: " << dbName
                                            << " due to: " << startStatus.toString();
            _setStatus_inlock({ErrorCodes::InitialSyncFailure, err});
            error() << err;
            break;  // exit for_each loop
        }

        // add cloner to list.
        _databaseCloners.push_back(dbCloner);
    }

    if (_databaseCloners.size() == 0) {
        if (_status.isOK()) {
            _succeed_inlock(&lk);
        } else {
            _fail_inlock(&lk, _status);
        }
    }
}

std::vector<std::shared_ptr<DatabaseCloner>> DatabasesCloner::_getDatabaseCloners() const {
    LockGuard lock(_mutex);
    return _databaseCloners;
}

RemoteCommandRetryScheduler* DatabasesCloner::_getListDatabasesScheduler() const {
    LockGuard lock(_mutex);
    return _listDBsScheduler.get();
}

void DatabasesCloner::_onEachDBCloneFinish(const Status& status, const std::string& name) {
    UniqueLock lk(_mutex);
    if (!status.isOK()) {
        warning() << "database '" << name << "' (" << (_stats.databasesCloned + 1) << " of "
                  << _databaseCloners.size() << ") clone failed due to " << status.toString();
        _fail_inlock(&lk, status);
        return;
    }

    if (StringData(name).equalCaseInsensitive("admin")) {
        LOG(1) << "Finished the 'admin' db, now calling isAdminDbValid.";
        // Do special checks for the admin database because of auth. collections.
        auto adminStatus = Status(ErrorCodes::NotYetInitialized, "");
        {
            // TODO: Move isAdminDbValid() out of the collection/database cloner code paths.
            OperationContext* txn = cc().getOperationContext();
            ServiceContext::UniqueOperationContext txnPtr;
            if (!txn) {
                txnPtr = cc().makeOperationContext();
                txn = txnPtr.get();
            }
            adminStatus = _storage->isAdminDbValid(txn);
        }
        if (!adminStatus.isOK()) {
            LOG(1) << "Validation failed on 'admin' db due to " << adminStatus;
            _fail_inlock(&lk, adminStatus);
            return;
        }
    }

    _stats.databasesCloned++;

    if (_stats.databasesCloned == _databaseCloners.size()) {
        _succeed_inlock(&lk);
        return;
    }

    // Start next database cloner.
    auto&& dbCloner = _databaseCloners[_stats.databasesCloned];
    auto startStatus = dbCloner->startup();
    if (!startStatus.isOK()) {
        warning() << "failed to schedule database '" << name << "' ("
                  << (_stats.databasesCloned + 1) << " of " << _databaseCloners.size()
                  << ") due to " << startStatus.toString();
        _fail_inlock(&lk, startStatus);
        return;
    }
}

void DatabasesCloner::_fail_inlock(UniqueLock* lk, Status status) {
    LOG(3) << "DatabasesCloner::_fail_inlock called";
    if (!_active) {
        return;
    }

    _setStatus_inlock(status);
    // TODO: shutdown outstanding work, like any cloners active
    auto finish = _finishFn;
    lk->unlock();

    LOG(3) << "DatabasesCloner - calling _finishFn with status: " << _status;
    finish(status);

    lk->lock();
    _active = false;
}

void DatabasesCloner::_succeed_inlock(UniqueLock* lk) {
    LOG(3) << "DatabasesCloner::_succeed_inlock called";
    const auto status = Status::OK();
    _setStatus_inlock(status);
    auto finish = _finishFn;
    lk->unlock();

    LOG(3) << "DatabasesCloner - calling _finishFn with status OK";
    finish(status);

    lk->lock();
    _active = false;
}

void DatabasesCloner::_setStatus_inlock(Status s) {
    // Only set the first time called, all subsequent failures are not recorded --only first.
    if (!s.isOK() && (_status.isOK() || _status == ErrorCodes::NotYetInitialized)) {
        LOG(1) << "setting DatabasesCloner status to " << s;
        _status = s;
    }
}

}  // namespace repl
}  // namespace mongo
