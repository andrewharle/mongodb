
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

#include "mongo/db/sessions_collection_sharded.h"

#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/sessions_collection_rs.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_find.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/cluster_write.h"

namespace mongo {

namespace {

BSONObj lsidQuery(const LogicalSessionId& lsid) {
    return BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON());
}

}  // namespace

Status SessionsCollectionSharded::_checkCacheForSessionsCollection(OperationContext* opCtx) {
    // If the sharding state is not yet initialized, fail.
    if (!Grid::get(opCtx)->isShardingInitialized()) {
        return {ErrorCodes::ShardingStateNotInitialized, "sharding state is not yet initialized"};
    }

    // If the collection doesn't exist, fail. Only the config servers generate it.
    auto res = Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
        opCtx, SessionsCollection::kSessionsNamespaceString);
    if (!res.isOK()) {
        return res.getStatus();
    }

    auto routingInfo = res.getValue();
    if (routingInfo.cm()) {
        return Status::OK();
    }

    return {ErrorCodes::NamespaceNotFound, "config.system.sessions does not exist"};
}

Status SessionsCollectionSharded::setupSessionsCollection(OperationContext* opCtx) {
    return checkSessionsCollectionExists(opCtx);
}

Status SessionsCollectionSharded::checkSessionsCollectionExists(OperationContext* opCtx) {
    return _checkCacheForSessionsCollection(opCtx);
}

Status SessionsCollectionSharded::refreshSessions(OperationContext* opCtx,
                                                  const LogicalSessionRecordSet& sessions) {
    auto send = [&](BSONObj toSend) {
        auto opMsg =
            OpMsgRequest::fromDBAndBody(SessionsCollection::kSessionsNamespaceString.db(), toSend);
        auto request = BatchedCommandRequest::parseUpdate(opMsg);

        BatchedCommandResponse response;
        BatchWriteExecStats stats;

        ClusterWriter::write(opCtx, request, &stats, &response);
        return response.toStatus();
    };

    return doRefresh(kSessionsNamespaceString, sessions, send);
}

Status SessionsCollectionSharded::removeRecords(OperationContext* opCtx,
                                                const LogicalSessionIdSet& sessions) {
    auto send = [&](BSONObj toSend) {
        auto opMsg =
            OpMsgRequest::fromDBAndBody(SessionsCollection::kSessionsNamespaceString.db(), toSend);
        auto request = BatchedCommandRequest::parseDelete(opMsg);

        BatchedCommandResponse response;
        BatchWriteExecStats stats;

        ClusterWriter::write(opCtx, request, &stats, &response);
        return response.toStatus();
    };

    return doRemove(kSessionsNamespaceString, sessions, send);
}

StatusWith<LogicalSessionIdSet> SessionsCollectionSharded::findRemovedSessions(
    OperationContext* opCtx, const LogicalSessionIdSet& sessions) {

    auto send = [&](BSONObj toSend) -> StatusWith<BSONObj> {
        auto qr = QueryRequest::makeFromFindCommand(
            SessionsCollection::kSessionsNamespaceString, toSend, false);
        if (!qr.isOK()) {
            return qr.getStatus();
        }

        const boost::intrusive_ptr<ExpressionContext> expCtx;
        auto cq = CanonicalQuery::canonicalize(opCtx,
                                               std::move(qr.getValue()),
                                               expCtx,
                                               ExtensionsCallbackNoop(),
                                               MatchExpressionParser::kBanAllSpecialFeatures);
        if (!cq.isOK()) {
            return cq.getStatus();
        }

        // Do the work to generate the first batch of results. This blocks waiting to get responses
        // from the shard(s).
        std::vector<BSONObj> batch;
        CursorId cursorId;
        try {
            cursorId = ClusterFind::runQuery(
                opCtx, *cq.getValue(), ReadPreferenceSetting::get(opCtx), &batch);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        BSONObjBuilder result;
        CursorResponseBuilder firstBatch(/*firstBatch*/ true, &result);
        for (const auto& obj : batch) {
            firstBatch.append(obj);
        }
        firstBatch.done(cursorId, SessionsCollection::kSessionsNamespaceString.ns());

        return result.obj();
    };

    return doFetch(kSessionsNamespaceString, sessions, send);
}

Status SessionsCollectionSharded::removeTransactionRecords(OperationContext* opCtx,
                                                           const LogicalSessionIdSet& sessions) {
    return SessionsCollectionRS::removeTransactionRecordsHelper(opCtx, sessions);
}

}  // namespace mongo
