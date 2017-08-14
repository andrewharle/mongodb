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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/s/commands/cluster_commands_common.h"

#include "mongo/db/commands.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/parallel.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/version_manager.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::string;

namespace {

bool forceRemoteCheckShardVersionCB(OperationContext* opCtx, const string& ns) {
    const NamespaceString nss(ns);

    if (!nss.isValid()) {
        return false;
    }

    // This will force the database catalog entry to be reloaded
    Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(nss);

    auto routingInfoStatus = Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss);
    if (!routingInfoStatus.isOK()) {
        return false;
    }

    auto& routingInfo = routingInfoStatus.getValue();

    return routingInfo.cm() != nullptr;
}

}  // namespace

Future::CommandResult::CommandResult(const string& server,
                                     const string& db,
                                     const BSONObj& cmd,
                                     int options,
                                     DBClientBase* conn,
                                     bool useShardedConn)
    : _server(server),
      _db(db),
      _options(options),
      _cmd(cmd),
      _conn(conn),
      _useShardConn(useShardedConn),
      _done(false) {
    init();
}

void Future::CommandResult::init() {
    try {
        if (!_conn) {
            if (_useShardConn) {
                _connHolder.reset(new ShardConnection(
                    uassertStatusOK(ConnectionString::parse(_server)), "", NULL));
            } else {
                _connHolder.reset(new ScopedDbConnection(_server));
            }

            _conn = _connHolder->get();
        }

        if (_conn->lazySupported()) {
            _cursor.reset(
                new DBClientCursor(_conn, _db + ".$cmd", _cmd, -1 /*limit*/, 0, NULL, _options, 0));
            _cursor->initLazy();
        } else {
            _done = true;  // we set _done first because even if there is an error we're done
            _ok = _conn->runCommand(_db, _cmd, _res, _options);
        }
    } catch (std::exception& e) {
        error() << "Future::spawnCommand (part 1) exception: " << redact(e.what());
        _ok = false;
        _done = true;
    }
}

bool Future::CommandResult::join(OperationContext* opCtx, int maxRetries) {
    if (_done) {
        return _ok;
    }

    _ok = false;

    for (int i = 1; i <= maxRetries; i++) {
        try {
            bool retry = false;
            bool finished = _cursor->initLazyFinish(retry);

            // Shouldn't need to communicate with server any more
            if (_connHolder)
                _connHolder->done();

            uassert(
                14812, str::stream() << "Error running command on server: " << _server, finished);
            massert(14813, "Command returned nothing", _cursor->more());

            // Rethrow stale config errors stored in this cursor for correct handling
            throwCursorStale(_cursor.get());

            _res = _cursor->nextSafe();
            _ok = _res["ok"].trueValue();

            break;
        } catch (const RecvStaleConfigException& e) {
            verify(versionManager.isVersionableCB(_conn));

            // For legacy reasons, we may not always have a namespace :-(
            string staleNS = e.getns();
            if (staleNS.size() == 0)
                staleNS = _db;

            if (i >= maxRetries) {
                error() << "Future::spawnCommand (part 2) stale config exception"
                        << causedBy(redact(e));
                throw e;
            }

            if (i >= maxRetries / 2) {
                if (!forceRemoteCheckShardVersionCB(opCtx, staleNS)) {
                    error() << "Future::spawnCommand (part 2) no config detected"
                            << causedBy(redact(e));
                    throw e;
                }
            }

            // We may not always have a collection, since we don't know from a generic command what
            // collection is supposed to be acted on, if any
            if (nsGetCollection(staleNS).size() == 0) {
                warning() << "no collection namespace in stale config exception "
                          << "for lazy command " << redact(_cmd) << ", could not refresh "
                          << staleNS;
            } else {
                versionManager.checkShardVersionCB(opCtx, _conn, staleNS, false, 1);
            }

            LOG(i > 1 ? 0 : 1) << "retrying lazy command" << causedBy(redact(e));

            verify(_conn->lazySupported());
            _done = false;
            init();
            continue;
        } catch (std::exception& e) {
            error() << "Future::spawnCommand (part 2) exception: " << causedBy(redact(e.what()));
            break;
        }
    }

    _done = true;
    return _ok;
}

shared_ptr<Future::CommandResult> Future::spawnCommand(const string& server,
                                                       const string& db,
                                                       const BSONObj& cmd,
                                                       int options,
                                                       DBClientBase* conn,
                                                       bool useShardConn) {
    shared_ptr<Future::CommandResult> res(
        new Future::CommandResult(server, db, cmd, options, conn, useShardConn));
    return res;
}

int getUniqueCodeFromCommandResults(const std::vector<Strategy::CommandResult>& results) {
    int commonErrCode = -1;
    for (std::vector<Strategy::CommandResult>::const_iterator it = results.begin();
         it != results.end();
         ++it) {
        // Only look at shards with errors.
        if (!it->result["ok"].trueValue()) {
            int errCode = it->result["code"].numberInt();

            if (commonErrCode == -1) {
                commonErrCode = errCode;
            } else if (commonErrCode != errCode) {
                // At least two shards with errors disagree on the error code
                commonErrCode = 0;
            }
        }
    }

    // If no error encountered or shards with errors disagree on the error code, return 0
    if (commonErrCode == -1 || commonErrCode == 0) {
        return 0;
    }

    // Otherwise, shards with errors agree on the error code; return that code
    return commonErrCode;
}

bool appendEmptyResultSet(BSONObjBuilder& result, Status status, const std::string& ns) {
    invariant(!status.isOK());

    if (status == ErrorCodes::NamespaceNotFound) {
        // Old style reply
        result << "result" << BSONArray();

        // New (command) style reply
        appendCursorResponseObject(0LL, ns, BSONArray(), &result);

        return true;
    }

    return Command::appendCommandStatus(result, status);
}

std::vector<NamespaceString> getAllShardedCollectionsForDb(OperationContext* opCtx,
                                                           StringData dbName) {
    const auto dbNameStr = dbName.toString();

    std::vector<CollectionType> collectionsOnConfig;
    uassertStatusOK(Grid::get(opCtx)->catalogClient(opCtx)->getCollections(
        opCtx, &dbNameStr, &collectionsOnConfig, nullptr));

    std::vector<NamespaceString> collectionsToReturn;
    for (const auto& coll : collectionsOnConfig) {
        if (coll.getDropped())
            continue;

        collectionsToReturn.push_back(coll.getNs());
    }

    return collectionsToReturn;
}

CachedCollectionRoutingInfo getShardedCollection(OperationContext* opCtx,
                                                 const NamespaceString& nss) {
    auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Collection " << nss.ns() << " is not sharded.",
            routingInfo.cm());

    return routingInfo;
}

StatusWith<CachedDatabaseInfo> createShardDatabase(OperationContext* opCtx, StringData dbName) {
    auto dbStatus = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName);
    if (dbStatus == ErrorCodes::NamespaceNotFound) {
        auto createDbStatus =
            Grid::get(opCtx)->catalogClient(opCtx)->createDatabase(opCtx, dbName.toString());
        if (createDbStatus.isOK() || createDbStatus == ErrorCodes::NamespaceExists) {
            dbStatus = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName);
        } else {
            dbStatus = createDbStatus;
        }
    }

    if (dbStatus.isOK()) {
        return dbStatus;
    }

    return {dbStatus.getStatus().code(),
            str::stream() << "Database " << dbName << " not found due to "
                          << dbStatus.getStatus().reason()};
}

}  // namespace mongo
