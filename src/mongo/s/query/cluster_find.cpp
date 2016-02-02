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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_find.h"

#include <set>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_explain.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

static const BSONObj kSortKeyMetaProjection = BSON("$meta"
                                                   << "sortKey");

// We must allow some amount of overhead per result document, since when we make a cursor response
// the documents are elements of a BSONArray. The overhead is 1 byte/doc for the type + 1 byte/doc
// for the field name's null terminator + 1 byte per digit in the array index. The index can be no
// more than 8 decimal digits since the response is at most 16MB, and 16 * 1024 * 1024 < 1 * 10^8.
static const int kPerDocumentOverheadBytesUpperBound = 10;

/**
 * Given the LiteParsedQuery 'lpq' being executed by mongos, returns a copy of the query which is
 * suitable for forwarding to the targeted hosts.
 */
std::unique_ptr<LiteParsedQuery> transformQueryForShards(const LiteParsedQuery& lpq) {
    // If there is a limit, we forward the sum of the limit and the skip.
    boost::optional<long long> newLimit;
    if (lpq.getLimit()) {
        newLimit = *lpq.getLimit() + lpq.getSkip().value_or(0);
    }

    // Similarly, if nToReturn is set, we forward the sum of nToReturn and the skip.
    boost::optional<long long> newNToReturn;
    if (lpq.getNToReturn()) {
        // !wantMore and ntoreturn mean the same as !wantMore and limit, so perform the conversion.
        if (!lpq.wantMore()) {
            newLimit = *lpq.getNToReturn() + lpq.getSkip().value_or(0);
        } else {
            newNToReturn = *lpq.getNToReturn() + lpq.getSkip().value_or(0);
        }
    }

    // If there is a sort other than $natural, we send a sortKey meta-projection to the remote node.
    BSONObj newProjection = lpq.getProj();
    if (!lpq.getSort().isEmpty() && !lpq.getSort()["$natural"]) {
        BSONObjBuilder projectionBuilder;
        projectionBuilder.appendElements(lpq.getProj());
        projectionBuilder.append(ClusterClientCursorParams::kSortKeyField, kSortKeyMetaProjection);
        newProjection = projectionBuilder.obj();
    }

    return LiteParsedQuery::makeAsFindCmd(lpq.nss(),
                                          lpq.getFilter(),
                                          newProjection,
                                          lpq.getSort(),
                                          lpq.getHint(),
                                          lpq.getReadConcern(),
                                          boost::none,  // Don't forward skip.
                                          newLimit,
                                          lpq.getBatchSize(),
                                          newNToReturn,
                                          lpq.wantMore(),
                                          lpq.isExplain(),
                                          lpq.getComment(),
                                          lpq.getMaxScan(),
                                          lpq.getMaxTimeMS(),
                                          lpq.getMin(),
                                          lpq.getMax(),
                                          lpq.returnKey(),
                                          lpq.showRecordId(),
                                          lpq.isSnapshot(),
                                          lpq.hasReadPref(),
                                          lpq.isTailable(),
                                          lpq.isSlaveOk(),
                                          lpq.isOplogReplay(),
                                          lpq.isNoCursorTimeout(),
                                          lpq.isAwaitData(),
                                          lpq.isAllowPartialResults());
}

/**
 * Runs a find command against the "config" shard in SyncClusterConnection (SCCC) mode. Special
 * handling is required for SCCC since the config shard's NS targeter is only available if the
 * config servers are in CSRS mode.
 *
 * 'query' is the query to run against the config shard. 'shard' must represent the config shard.
 *
 * On success, fills out 'results' with the documents returned from the config shard and returns the
 * cursor id which should be handed back to the client.
 *
 * TODO: This should not be required for 3.4, since the config server mode must be config server
 * replica set (CSRS) in order to upgrade.
 */
StatusWith<CursorId> runConfigServerQuerySCCC(const CanonicalQuery& query,
                                              const Shard& shard,
                                              std::vector<BSONObj>* results) {
    BSONObj findCommand = query.getParsed().asFindCommand();

    // XXX: This is a temporary hack. We use ScopedDbConnection and query the $cmd namespace
    // explicitly because this gives us the particular host that the command ran on via
    // originalHost(). We need to know the host that the remote cursor was established on in order
    // to issue getMore or killCursors operations against this remote cursor.
    ScopedDbConnection conn(shard.getConnString());
    auto cursor = conn->query(str::stream() << query.nss().db() << ".$cmd",
                              findCommand,
                              -1,       // nToReturn
                              0,        // nToSkip
                              nullptr,  // fieldsToReturn
                              0);       // options
    if (!cursor || !cursor->more()) {
        return {ErrorCodes::OperationFailed, "failed to run find command against config shard"};
    }
    BSONObj result = cursor->nextSafe().getOwned();
    conn.done();

    auto status = Command::getStatusFromCommandResult(result);
    if (ErrorCodes::SendStaleConfig == status || ErrorCodes::RecvStaleConfig == status) {
        throw RecvStaleConfigException("find command failed because of stale config", result);
    }

    auto executorPool = grid.shardRegistry()->getExecutorPool();
    auto transformedResult = storePossibleCursor(HostAndPort(cursor->originalHost()),
                                                 result,
                                                 executorPool->getArbitraryExecutor(),
                                                 grid.getCursorManager());
    if (!transformedResult.isOK()) {
        return transformedResult.getStatus();
    }

    auto outgoingCursorResponse = CursorResponse::parseFromBSON(transformedResult.getValue());
    if (!outgoingCursorResponse.isOK()) {
        return outgoingCursorResponse.getStatus();
    }

    for (const auto& doc : outgoingCursorResponse.getValue().getBatch()) {
        results->push_back(doc.getOwned());
    }

    return outgoingCursorResponse.getValue().getCursorId();
}

StatusWith<CursorId> runQueryWithoutRetrying(OperationContext* txn,
                                             const CanonicalQuery& query,
                                             const ReadPreferenceSetting& readPref,
                                             ChunkManager* chunkManager,
                                             std::shared_ptr<Shard> primary,
                                             std::vector<BSONObj>* results) {
    auto shardRegistry = grid.shardRegistry();

    // Get the set of shards on which we will run the query.
    std::vector<std::shared_ptr<Shard>> shards;
    if (primary) {
        shards.emplace_back(std::move(primary));
    } else {
        invariant(chunkManager);

        std::set<ShardId> shardIds;
        chunkManager->getShardIdsForQuery(txn, query.getParsed().getFilter(), &shardIds);

        for (auto id : shardIds) {
            shards.emplace_back(shardRegistry->getShard(txn, id));
        }
    }

    ClusterClientCursorParams params(query.nss(), readPref);
    params.limit = query.getParsed().getLimit();
    params.batchSize = query.getParsed().getEffectiveBatchSize();
    params.skip = query.getParsed().getSkip();
    params.isTailable = query.getParsed().isTailable();
    params.isAwaitData = query.getParsed().isAwaitData();
    params.isAllowPartialResults = query.getParsed().isAllowPartialResults();

    // This is the batchSize passed to each subsequent getMore command issued by the cursor. We
    // usually use the batchSize associated with the initial find, but as it is illegal to send a
    // getMore with a batchSize of 0, we set it to use the default batchSize logic.
    if (params.batchSize && *params.batchSize == 0) {
        params.batchSize = boost::none;
    }

    // $natural sort is actually a hint to use a collection scan, and shouldn't be treated like a
    // sort on mongos. Including a $natural anywhere in the sort spec results in the whole sort
    // being considered a hint to use a collection scan.
    if (!query.getParsed().getSort().hasField("$natural")) {
        params.sort = FindCommon::transformSortSpec(query.getParsed().getSort());
    }

    // Tailable cursors can't have a sort, which should have already been validated.
    invariant(params.sort.isEmpty() || !params.isTailable);

    const auto lpqToForward = transformQueryForShards(query.getParsed());

    // Use read pref to target a particular host from each shard. Also construct the find command
    // that we will forward to each shard.
    for (const auto& shard : shards) {
        // The unified targeting logic only works for config server replica sets, so we need special
        // handling for querying config server content with legacy 3-host config servers.
        if (shard->isConfig() && shard->getConnString().type() == ConnectionString::SYNC) {
            invariant(shards.size() == 1U);
            return runConfigServerQuerySCCC(query, *shard, results);
        }

        // Build the find command, and attach shard version if necessary.
        BSONObjBuilder cmdBuilder;
        lpqToForward->asFindCommand(&cmdBuilder);

        if (chunkManager) {
            ChunkVersion version(chunkManager->getVersion(shard->getId()));
            version.appendForCommands(&cmdBuilder);
        } else if (!query.nss().isOnInternalDb()) {
            ChunkVersion version(ChunkVersion::UNSHARDED());
            version.appendForCommands(&cmdBuilder);
        }

        params.remotes.emplace_back(shard->getId(), cmdBuilder.obj());
    }

    auto ccc = ClusterClientCursorImpl::make(
        shardRegistry->getExecutorPool()->getArbitraryExecutor(), std::move(params));

    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    int bytesBuffered = 0;
    while (!FindCommon::enoughForFirstBatch(query.getParsed(), results->size(), bytesBuffered)) {
        auto next = ccc->next();
        if (!next.isOK()) {
            return next.getStatus();
        }

        if (!next.getValue()) {
            // We reached end-of-stream. If the cursor is not tailable, then we mark it as
            // exhausted. If it is tailable, usually we keep it open (i.e. "NotExhausted") even
            // when we reach end-of-stream. However, if all the remote cursors are exhausted, there
            // is no hope of returning data and thus we need to close the mongos cursor as well.
            if (!ccc->isTailable() || ccc->remotesExhausted()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
            break;
        }

        // If adding this object will cause us to exceed the BSON size limit, then we stash it for
        // later. By using BSONObjMaxUserSize, we ensure that there is enough room for the
        // "envelope" (e.g. the "ns" and "id" fields included in the response) before exceeding
        // BSONObjMaxInternalSize.
        int sizeEstimate = bytesBuffered + next.getValue()->objsize() +
            ((results->size() + 1U) * kPerDocumentOverheadBytesUpperBound);
        if (sizeEstimate > BSONObjMaxUserSize && !results->empty()) {
            ccc->queueResult(*next.getValue());
            break;
        }

        // Add doc to the batch.
        bytesBuffered += next.getValue()->objsize();
        results->push_back(std::move(*next.getValue()));
    }

    if (!query.getParsed().wantMore() && !ccc->isTailable()) {
        cursorState = ClusterCursorManager::CursorState::Exhausted;
    }

    // If the cursor is exhausted, then there are no more results to return and we don't need to
    // allocate a cursor id.
    if (cursorState == ClusterCursorManager::CursorState::Exhausted) {
        return CursorId(0);
    }

    // Register the cursor with the cursor manager.
    auto cursorManager = grid.getCursorManager();
    const auto cursorType = chunkManager ? ClusterCursorManager::CursorType::NamespaceSharded
                                         : ClusterCursorManager::CursorType::NamespaceNotSharded;
    const auto cursorLifetime = query.getParsed().isNoCursorTimeout()
        ? ClusterCursorManager::CursorLifetime::Immortal
        : ClusterCursorManager::CursorLifetime::Mortal;
    return cursorManager->registerCursor(
        ccc.releaseCursor(), query.nss(), cursorType, cursorLifetime);
}

}  // namespace

const size_t ClusterFind::kMaxStaleConfigRetries = 10;

StatusWith<CursorId> ClusterFind::runQuery(OperationContext* txn,
                                           const CanonicalQuery& query,
                                           const ReadPreferenceSetting& readPref,
                                           std::vector<BSONObj>* results) {
    invariant(results);

    // Projection on the reserved sort key field is illegal in mongos.
    if (query.getParsed().getProj().hasField(ClusterClientCursorParams::kSortKeyField)) {
        return {ErrorCodes::BadValue,
                str::stream() << "Projection contains illegal field '"
                              << ClusterClientCursorParams::kSortKeyField
                              << "': " << query.getParsed().getProj()};
    }

    auto dbConfig = grid.catalogCache()->getDatabase(txn, query.nss().db().toString());
    if (dbConfig.getStatus() == ErrorCodes::NamespaceNotFound) {
        // If the database doesn't exist, we successfully return an empty result set without
        // creating a cursor.
        return CursorId(0);
    } else if (!dbConfig.isOK()) {
        return dbConfig.getStatus();
    }

    std::shared_ptr<ChunkManager> chunkManager;
    std::shared_ptr<Shard> primary;
    dbConfig.getValue()->getChunkManagerOrPrimary(txn, query.nss().ns(), chunkManager, primary);

    // Re-target and re-send the initial find command to the shards until we have established the
    // shard version.
    for (size_t retries = 1; retries <= kMaxStaleConfigRetries; ++retries) {
        auto cursorId = runQueryWithoutRetrying(
            txn, query, readPref, chunkManager.get(), std::move(primary), results);
        if (cursorId.isOK()) {
            return cursorId;
        }
        auto status = std::move(cursorId.getStatus());

        if (!ErrorCodes::isStaleShardingError(status.code())) {
            // Errors other than receiving a stale metadata message from MongoD are fatal to the
            // operation. Network errors and replication retries happen at the level of the
            // AsyncResultsMerger.
            return status;
        }

        LOG(1) << "Received error status for query " << query.toStringShort() << " on attempt "
               << retries << " of " << kMaxStaleConfigRetries << ": " << status;

        const bool staleEpoch = (status == ErrorCodes::StaleEpoch);
        if (staleEpoch) {
            if (!dbConfig.getValue()->reload(txn)) {
                // If the reload failed that means the database wasn't found, so successfully return
                // an empty result set without creating a cursor.
                return CursorId(0);
            }
        }
        chunkManager =
            dbConfig.getValue()->getChunkManagerIfExists(txn, query.nss().ns(), true, staleEpoch);
        if (!chunkManager) {
            dbConfig.getValue()->getChunkManagerOrPrimary(
                txn, query.nss().ns(), chunkManager, primary);
        }
    }

    return {ErrorCodes::StaleShardVersion,
            str::stream() << "Retried " << kMaxStaleConfigRetries
                          << " times without successfully establishing shard version."};
}

StatusWith<CursorResponse> ClusterFind::runGetMore(OperationContext* txn,
                                                   const GetMoreRequest& request) {
    auto cursorManager = grid.getCursorManager();

    auto pinnedCursor = cursorManager->checkOutCursor(request.nss, request.cursorid);
    if (!pinnedCursor.isOK()) {
        return pinnedCursor.getStatus();
    }
    invariant(request.cursorid == pinnedCursor.getValue().getCursorId());

    // If the fail point is enabled, busy wait until it is disabled.
    while (MONGO_FAIL_POINT(keepCursorPinnedDuringGetMore)) {
    }

    if (request.awaitDataTimeout) {
        auto status = pinnedCursor.getValue().setAwaitDataTimeout(*request.awaitDataTimeout);
        if (!status.isOK()) {
            return status;
        }
    }

    std::vector<BSONObj> batch;
    int bytesBuffered = 0;
    long long batchSize = request.batchSize.value_or(0);
    long long startingFrom = pinnedCursor.getValue().getNumReturnedSoFar();
    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    while (!FindCommon::enoughForGetMore(batchSize, batch.size(), bytesBuffered)) {
        auto next = pinnedCursor.getValue().next();
        if (!next.isOK()) {
            return next.getStatus();
        }

        if (!next.getValue()) {
            // We reached end-of-stream.
            if (!pinnedCursor.getValue().isTailable()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
            break;
        }

        // If adding this object will cause us to exceed the BSON size limit, then we stash it for
        // later. By using BSONObjMaxUserSize, we ensure that there is enough room for the
        // "envelope" (e.g. the "ns" and "id" fields included in the response) before exceeding
        // BSONObjMaxInternalSize.
        int sizeEstimate = bytesBuffered + next.getValue()->objsize() +
            ((batch.size() + 1U) * kPerDocumentOverheadBytesUpperBound);
        if (sizeEstimate > BSONObjMaxUserSize && !batch.empty()) {
            pinnedCursor.getValue().queueResult(*next.getValue());
            break;
        }

        // Add doc to the batch.
        bytesBuffered += next.getValue()->objsize();
        batch.push_back(std::move(*next.getValue()));
    }

    // Transfer ownership of the cursor back to the cursor manager.
    pinnedCursor.getValue().returnCursor(cursorState);

    CursorId idToReturn = (cursorState == ClusterCursorManager::CursorState::Exhausted)
        ? CursorId(0)
        : request.cursorid;
    return CursorResponse(request.nss, idToReturn, std::move(batch), startingFrom);
}

StatusWith<ReadPreferenceSetting> ClusterFind::extractUnwrappedReadPref(const BSONObj& cmdObj,
                                                                        const bool isSlaveOk) {
    BSONElement queryOptionsElt;
    auto status = bsonExtractTypedField(
        cmdObj, LiteParsedQuery::kUnwrappedReadPrefField, BSONType::Object, &queryOptionsElt);
    if (status.isOK()) {
        // There must be a nested object containing the read preference if there is a queryOptions
        // field.
        BSONObj queryOptionsObj = queryOptionsElt.Obj();
        invariant(queryOptionsObj[LiteParsedQuery::kWrappedReadPrefField].type() ==
                  BSONType::Object);
        BSONObj readPrefObj = queryOptionsObj[LiteParsedQuery::kWrappedReadPrefField].Obj();

        auto readPref = ReadPreferenceSetting::fromBSON(readPrefObj);
        if (!readPref.isOK()) {
            return readPref.getStatus();
        }
        return readPref.getValue();
    } else if (status != ErrorCodes::NoSuchKey) {
        return status;
    }

    // If there is no explicit read preference, the value we use depends on the setting of the slave
    // ok bit.
    ReadPreference pref =
        isSlaveOk ? mongo::ReadPreference::SecondaryPreferred : mongo::ReadPreference::PrimaryOnly;
    return ReadPreferenceSetting(pref, TagSet());
}

}  // namespace mongo
