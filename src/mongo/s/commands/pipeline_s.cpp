
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

#include "mongo/s/commands/pipeline_s.h"

#include "mongo/db/curop.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/router_exec_stage.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

namespace {
/**
 * Determines the single shard to which the given query will be targeted, and its associated
 * shardVersion. Throws if the query targets more than one shard.
 */
std::pair<ShardId, ChunkVersion> getSingleTargetedShardForQuery(
    OperationContext* opCtx, const CachedCollectionRoutingInfo& routingInfo, BSONObj query) {
    if (auto chunkMgr = routingInfo.cm()) {
        std::set<ShardId> shardIds;
        chunkMgr->getShardIdsForQuery(opCtx, query, CollationSpec::kSimpleSpec, &shardIds);
        uassert(ErrorCodes::ChangeStreamFatalError,
                str::stream() << "Unable to target lookup query to a single shard: "
                              << query.toString(),
                shardIds.size() == 1u);
        return {*shardIds.begin(), chunkMgr->getVersion(*shardIds.begin())};
    }

    return {routingInfo.db().primaryId(), ChunkVersion::UNSHARDED()};
}

/**
 * Returns the routing information for the namespace set on the passed ExpressionContext. Also
 * verifies that the ExpressionContext's UUID, if present, matches that of the routing table entry.
 */
StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfo(
    const intrusive_ptr<ExpressionContext>& expCtx) {
    auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
    auto swRoutingInfo = catalogCache->getCollectionRoutingInfo(expCtx->opCtx, expCtx->ns);
    // Additionally check that the ExpressionContext's UUID matches the collection routing info.
    if (swRoutingInfo.isOK() && expCtx->uuid && swRoutingInfo.getValue().cm()) {
        if (!swRoutingInfo.getValue().cm()->uuidMatches(*expCtx->uuid)) {
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "The UUID of collection " << expCtx->ns.ns()
                                  << " changed; it may have been dropped and re-created."};
        }
    }
    return swRoutingInfo;
}

}  // namespace

boost::optional<Document> PipelineS::MongoSInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    UUID collectionUUID,
    const Document& filter,
    boost::optional<BSONObj> readConcern) {
    auto foreignExpCtx = expCtx->copyWith(nss, collectionUUID);

    // Create the find command to be dispatched to the shard in order to return the post-change
    // document.
    auto filterObj = filter.toBson();
    BSONObjBuilder cmdBuilder;
    bool findCmdIsByUuid(foreignExpCtx->uuid);
    if (findCmdIsByUuid) {
        foreignExpCtx->uuid->appendToBuilder(&cmdBuilder, "find");
    } else {
        cmdBuilder.append("find", nss.coll());
    }
    cmdBuilder.append("filter", filterObj);
    cmdBuilder.append("comment", expCtx->comment);
    if (readConcern) {
        cmdBuilder.append(repl::ReadConcernArgs::kReadConcernFieldName, *readConcern);
    }

    auto shardResult = std::vector<RemoteCursor>();
    auto findCmd = cmdBuilder.obj();
    size_t numAttempts = 0;
    while (++numAttempts <= kMaxNumStaleVersionRetries) {
        // Verify that the collection exists, with the correct UUID.
        auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
        auto swRoutingInfo = getCollectionRoutingInfo(foreignExpCtx);
        if (swRoutingInfo == ErrorCodes::NamespaceNotFound) {
            return boost::none;
        }
        auto routingInfo = uassertStatusOK(std::move(swRoutingInfo));
        if (findCmdIsByUuid && routingInfo.cm()) {
            // Find by UUID and shard versioning do not work together (SERVER-31946).  In the
            // sharded case we've already checked the UUID, so find by namespace is safe.  In the
            // unlikely case that the collection has been deleted and a new collection with the same
            // name created through a different mongos, the shard version will be detected as stale,
            // as shard versions contain an 'epoch' field unique to the collection.
            findCmd = findCmd.addField(BSON("find" << nss.coll()).firstElement());
            findCmdIsByUuid = false;
        }

        // Get the ID and version of the single shard to which this query will be sent.
        auto shardInfo = getSingleTargetedShardForQuery(expCtx->opCtx, routingInfo, filterObj);

        // Dispatch the request. This will only be sent to a single shard and only a single result
        // will be returned. The 'establishCursors' method conveniently prepares the result into a
        // cursor response for us.
        try {
            shardResult = establishCursors(
                expCtx->opCtx,
                Grid::get(expCtx->opCtx)->getExecutorPool()->getArbitraryExecutor(),
                nss,
                ReadPreferenceSetting::get(expCtx->opCtx),
                {{shardInfo.first, appendShardVersion(findCmd, shardInfo.second)}},
                false);
            break;
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // If it's an unsharded collection which has been deleted and re-created, we may get a
            // NamespaceNotFound error when looking up by UUID.
            return boost::none;
        } catch (const ExceptionForCat<ErrorCategory::StaleShardVersionError>&) {
            // If we hit a stale shardVersion exception, invalidate the routing table cache.
            catalogCache->onStaleShardVersion(std::move(routingInfo));
            continue;  // Try again if allowed.
        }
        break;  // Success!
    }

    invariant(shardResult.size() == 1u);

    auto& cursor = shardResult.front().getCursorResponse();
    auto& batch = cursor.getBatch();

    // We should have at most 1 result, and the cursor should be exhausted.
    uassert(ErrorCodes::ChangeStreamFatalError,
            str::stream() << "Shard cursor was unexpectedly open after lookup: "
                          << shardResult.front().getHostAndPort()
                          << ", id: "
                          << cursor.getCursorId(),
            cursor.getCursorId() == 0);
    uassert(ErrorCodes::ChangeStreamFatalError,
            str::stream() << "found more than one document matching " << filter.toString() << " ["
                          << batch.begin()->toString()
                          << ", "
                          << std::next(batch.begin())->toString()
                          << "]",
            batch.size() <= 1u);

    return (!batch.empty() ? Document(batch.front()) : boost::optional<Document>{});
}

BSONObj PipelineS::MongoSInterface::_reportCurrentOpForClient(
    OperationContext* opCtx, Client* client, CurrentOpTruncateMode truncateOps) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(
        opCtx, client, (truncateOps == CurrentOpTruncateMode::kTruncateOps), &builder);

    return builder.obj();
}

std::vector<GenericCursor> PipelineS::MongoSInterface::getCursors(
    const intrusive_ptr<ExpressionContext>& expCtx) const {
    invariant(hasGlobalServiceContext());
    auto cursorManager = Grid::get(expCtx->opCtx->getServiceContext())->getCursorManager();
    invariant(cursorManager);
    return cursorManager->getAllCursors();
}

}  // namespace mongo
