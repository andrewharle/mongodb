
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

#include "mongo/platform/basic.h"

#include "mongo/db/sessions_collection_rs.h"

#include <boost/optional.hpp>
#include <utility>

#include "mongo/client/connection_string.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/query.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace {

BSONObj lsidQuery(const LogicalSessionId& lsid) {
    return BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON());
}

Status makePrimaryConnection(OperationContext* opCtx, boost::optional<ScopedDbConnection>* conn) {
    auto coord = mongo::repl::ReplicationCoordinator::get(opCtx);
    auto config = coord->getConfig();
    if (!config.isInitialized()) {
        return {ErrorCodes::NotYetInitialized, "Replication has not yet been configured"};
    }

    // Find the primary
    RemoteCommandTargeterFactoryImpl factory;
    auto targeter = factory.create(config.getConnectionString());
    auto res = targeter->findHost(opCtx, ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    if (!res.isOK()) {
        return res.getStatus();
    }

    auto hostname = res.getValue().toString();

    // Make a connection to the primary, auth, then send
    try {
        conn->emplace(hostname);
        if (isInternalAuthSet()) {
            (*conn)->get()->auth(getInternalUserAuthParams());
        }
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

template <typename Callback>
auto runIfStandaloneOrPrimary(const NamespaceString& ns, OperationContext* opCtx, Callback callback)
    -> boost::optional<decltype(std::declval<Callback>()())> {
    bool isStandaloneOrPrimary;
    {
        Lock::DBLock lk(opCtx, ns.db(), MODE_IS);
        Lock::CollectionLock lock(
            opCtx->lockState(), SessionsCollection::kSessionsNamespaceString.ns(), MODE_IS);

        auto coord = mongo::repl::ReplicationCoordinator::get(opCtx);

        // There is a window here where we may transition from Primary to
        // Secondary after we release the locks we take above. In this case,
        // the callback we run below may return a NotMaster error, or a stale
        // read. However, this is preferable to running the callback while
        // we hold locks, since that can lead to a deadlock.
        isStandaloneOrPrimary = coord->canAcceptWritesForDatabase(opCtx, ns.db());
    }

    if (isStandaloneOrPrimary) {
        return callback();
    }

    return boost::none;
}

template <typename Callback>
auto sendToPrimary(OperationContext* opCtx, Callback callback)
    -> decltype(std::declval<Callback>()(static_cast<DBClientBase*>(nullptr))) {
    boost::optional<ScopedDbConnection> conn;
    auto res = makePrimaryConnection(opCtx, &conn);
    if (!res.isOK()) {
        return res;
    }

    auto val = callback(conn->get());

    if (val.isOK()) {
        conn->done();
    } else {
        conn->kill();
    }

    return std::move(val);
}

template <typename LocalCallback, typename RemoteCallback>
auto dispatch(const NamespaceString& ns,
              OperationContext* opCtx,
              LocalCallback localCallback,
              RemoteCallback remoteCallback)
    -> decltype(std::declval<RemoteCallback>()(static_cast<DBClientBase*>(nullptr))) {
    // If we are the primary, write directly to ourself.
    auto result = runIfStandaloneOrPrimary(ns, opCtx, [&] { return localCallback(); });

    if (result) {
        return *result;
    }

    return sendToPrimary(opCtx, remoteCallback);
}

}  // namespace

Status SessionsCollectionRS::setupSessionsCollection(OperationContext* opCtx) {
    return dispatch(
        kSessionsNamespaceString,
        opCtx,
        [&] {
            auto existsStatus = checkSessionsCollectionExists(opCtx);
            if (existsStatus.isOK()) {
                return Status::OK();
            }

            DBDirectClient client(opCtx);
            BSONObj cmd;

            if (existsStatus.code() == ErrorCodes::IndexOptionsConflict) {
                cmd = generateCollModCmd();
            } else {
                // Creating the TTL index will auto-generate the collection.
                cmd = generateCreateIndexesCmd();
            }

            BSONObj info;
            if (!client.runCommand(kSessionsNamespaceString.db().toString(), cmd, info)) {
                return getStatusFromCommandResult(info);
            }

            return Status::OK();
        },
        [&](DBClientBase*) { return checkSessionsCollectionExists(opCtx); });
}

Status SessionsCollectionRS::checkSessionsCollectionExists(OperationContext* opCtx) {
    DBDirectClient client(opCtx);

    auto indexes = client.getIndexSpecs(kSessionsNamespaceString.toString());

    if (indexes.size() == 0u) {
        return Status{ErrorCodes::NamespaceNotFound, "config.system.sessions does not exist"};
    }

    auto index = std::find_if(indexes.begin(), indexes.end(), [](const BSONObj& index) {
        return index.getField("name").String() == kSessionsTTLIndex;
    });

    if (index == indexes.end()) {
        return Status{ErrorCodes::IndexNotFound,
                      "config.system.sessions does not have the required TTL index"};
    }

    if (!index->hasField("expireAfterSeconds") ||
        index->getField("expireAfterSeconds").Int() != (localLogicalSessionTimeoutMinutes * 60)) {
        return Status{
            ErrorCodes::IndexOptionsConflict,
            "config.system.sessions currently has the incorrect timeout for the TTL index"};
    }

    return Status::OK();
}

Status SessionsCollectionRS::refreshSessions(OperationContext* opCtx,
                                             const LogicalSessionRecordSet& sessions) {
    return dispatch(kSessionsNamespaceString,
                    opCtx,
                    [&] {
                        DBDirectClient client(opCtx);
                        return doRefresh(
                            kSessionsNamespaceString,
                            sessions,
                            makeSendFnForBatchWrite(kSessionsNamespaceString, &client));
                    },
                    [&](DBClientBase* client) {
                        return doRefresh(kSessionsNamespaceString,
                                         sessions,
                                         makeSendFnForBatchWrite(kSessionsNamespaceString, client));
                    });
}

Status SessionsCollectionRS::removeRecords(OperationContext* opCtx,
                                           const LogicalSessionIdSet& sessions) {
    return dispatch(kSessionsNamespaceString,
                    opCtx,
                    [&] {
                        DBDirectClient client(opCtx);
                        return doRemove(kSessionsNamespaceString,
                                        sessions,
                                        makeSendFnForBatchWrite(kSessionsNamespaceString, &client));
                    },
                    [&](DBClientBase* client) {
                        return doRemove(kSessionsNamespaceString,
                                        sessions,
                                        makeSendFnForBatchWrite(kSessionsNamespaceString, client));
                    });
}

StatusWith<LogicalSessionIdSet> SessionsCollectionRS::findRemovedSessions(
    OperationContext* opCtx, const LogicalSessionIdSet& sessions) {
    return dispatch(kSessionsNamespaceString,
                    opCtx,
                    [&] {
                        DBDirectClient client(opCtx);
                        return doFetch(kSessionsNamespaceString,
                                       sessions,
                                       makeFindFnForCommand(kSessionsNamespaceString, &client));
                    },
                    [&](DBClientBase* client) {
                        return doFetch(kSessionsNamespaceString,
                                       sessions,
                                       makeFindFnForCommand(kSessionsNamespaceString, client));
                    });
}

Status SessionsCollectionRS::removeTransactionRecords(OperationContext* opCtx,
                                                      const LogicalSessionIdSet& sessions) {
    return dispatch(
        NamespaceString::kSessionTransactionsTableNamespace,
        opCtx,
        [&] {
            DBDirectClient client(opCtx);
            return doRemove(NamespaceString::kSessionTransactionsTableNamespace,
                            sessions,
                            makeSendFnForBatchWrite(
                                NamespaceString::kSessionTransactionsTableNamespace, &client));
        },
        [](DBClientBase*) {
            return Status(ErrorCodes::NotMaster, "Not eligible to remove transaction records");
        });
}

Status SessionsCollectionRS::removeTransactionRecordsHelper(OperationContext* opCtx,
                                                            const LogicalSessionIdSet& sessions) {
    return SessionsCollectionRS{}.removeTransactionRecords(opCtx, sessions);
}

}  // namespace mongo
