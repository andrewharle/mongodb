
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/dbdirectclient.h"

#include <boost/core/swap.hpp>

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/wire_version.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::unique_ptr;
using std::string;

namespace {

class DirectClientScope {
    MONGO_DISALLOW_COPYING(DirectClientScope);

public:
    explicit DirectClientScope(OperationContext* opCtx)
        : _opCtx(opCtx), _prev(_opCtx->getClient()->isInDirectClient()) {
        _opCtx->getClient()->setInDirectClient(true);
    }

    ~DirectClientScope() {
        _opCtx->getClient()->setInDirectClient(_prev);
    }

private:
    OperationContext* const _opCtx;
    const bool _prev;
};

}  // namespace


DBDirectClient::DBDirectClient(OperationContext* opCtx) : _opCtx(opCtx) {
    _setServerRPCProtocols(rpc::supports::kAll);
}

bool DBDirectClient::isFailed() const {
    return false;
}

bool DBDirectClient::isStillConnected() {
    return true;
}

std::string DBDirectClient::toString() const {
    return "DBDirectClient";
}

std::string DBDirectClient::getServerAddress() const {
    return "localhost";  // TODO: should this have the port?
}

// Returned version should match the incoming connections restrictions.
int DBDirectClient::getMinWireVersion() {
    return WireSpec::instance().incomingExternalClient.minWireVersion;
}

// Returned version should match the incoming connections restrictions.
int DBDirectClient::getMaxWireVersion() {
    return WireSpec::instance().incomingExternalClient.maxWireVersion;
}

bool DBDirectClient::isReplicaSetMember() const {
    auto const* replCoord = repl::ReplicationCoordinator::get(_opCtx);
    return replCoord && replCoord->isReplEnabled();
}

ConnectionString::ConnectionType DBDirectClient::type() const {
    return ConnectionString::MASTER;
}

double DBDirectClient::getSoTimeout() const {
    return 0;
}

bool DBDirectClient::lazySupported() const {
    return false;
}

void DBDirectClient::setOpCtx(OperationContext* opCtx) {
    _opCtx = opCtx;
}

QueryOptions DBDirectClient::_lookupAvailableOptions() {
    // Exhaust mode is not available in DBDirectClient.
    return QueryOptions(DBClientBase::_lookupAvailableOptions() & ~QueryOption_Exhaust);
}

namespace {
DbResponse loopbackBuildResponse(OperationContext* const opCtx,
                                 LastError* lastError,
                                 Message& toSend) {
    DirectClientScope directClientScope(opCtx);
    boost::swap(*lastError, LastError::get(opCtx->getClient()));
    ON_BLOCK_EXIT([&] { boost::swap(*lastError, LastError::get(opCtx->getClient())); });

    LastError::get(opCtx->getClient()).startRequest();
    CurOp curOp(opCtx);

    toSend.header().setId(nextMessageId());
    toSend.header().setResponseToMsgId(0);
    return opCtx->getServiceContext()->getServiceEntryPoint()->handleRequest(opCtx, toSend);
}
}  // namespace

bool DBDirectClient::call(Message& toSend, Message& response, bool assertOk, string* actualServer) {
    auto dbResponse = loopbackBuildResponse(_opCtx, &_lastError, toSend);
    invariant(!dbResponse.response.empty());
    response = std::move(dbResponse.response);

    return true;
}

void DBDirectClient::say(Message& toSend, bool isRetry, string* actualServer) {
    auto dbResponse = loopbackBuildResponse(_opCtx, &_lastError, toSend);
    invariant(dbResponse.response.empty());
}

unique_ptr<DBClientCursor> DBDirectClient::query(const string& ns,
                                                 Query query,
                                                 int nToReturn,
                                                 int nToSkip,
                                                 const BSONObj* fieldsToReturn,
                                                 int queryOptions,
                                                 int batchSize) {
    return DBClientBase::query(
        ns, query, nToReturn, nToSkip, fieldsToReturn, queryOptions, batchSize);
}

unsigned long long DBDirectClient::count(
    const string& ns, const BSONObj& query, int options, int limit, int skip) {
    BSONObj cmdObj = _countCmd(ns, query, options, limit, skip);

    NamespaceString nsString(ns);

    auto result = CommandHelpers::runCommandDirectly(
        _opCtx, OpMsgRequest::fromDBAndBody(nsString.db(), std::move(cmdObj)));

    uassertStatusOK(getStatusFromCommandResult(result));
    return static_cast<unsigned long long>(result["n"].numberLong());
}

}  // namespace mongo
