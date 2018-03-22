/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/sharding_catalog_manager_impl.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

MONGO_FP_DECLARE(migrationCommitVersionError);

/**
 * Append min, max and version information from chunk to the buffer for logChange purposes.
 */
void appendShortVersion(BufBuilder* b, const ChunkType& chunk) {
    BSONObjBuilder bb(*b);
    bb.append(ChunkType::min(), chunk.getMin());
    bb.append(ChunkType::max(), chunk.getMax());
    if (chunk.isVersionSet())
        chunk.getVersion().addToBSON(bb, ChunkType::DEPRECATED_lastmod());
    bb.done();
}

BSONArray buildMergeChunksApplyOpsUpdates(const std::vector<ChunkType>& chunksToMerge,
                                          const ChunkVersion& mergeVersion) {
    BSONArrayBuilder updates;

    // Build an update operation to expand the first chunk into the newly merged chunk
    {
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);  // no upsert
        op.append("ns", ChunkType::ConfigNS);

        // expand first chunk into newly merged chunk
        ChunkType mergedChunk(chunksToMerge.front());
        mergedChunk.setMax(chunksToMerge.back().getMax());

        // fill in additional details for sending through applyOps
        mergedChunk.setVersion(mergeVersion);

        // add the new chunk information as the update object
        op.append("o", mergedChunk.toBSON());

        // query object
        op.append("o2", BSON(ChunkType::name(mergedChunk.getName())));

        updates.append(op.obj());
    }

    // Build update operations to delete the rest of the chunks to be merged. Remember not
    // to delete the first chunk we're expanding
    for (size_t i = 1; i < chunksToMerge.size(); ++i) {
        BSONObjBuilder op;
        op.append("op", "d");
        op.append("ns", ChunkType::ConfigNS);

        op.append("o", BSON(ChunkType::name(chunksToMerge[i].getName())));

        updates.append(op.obj());
    }

    return updates.arr();
}

BSONArray buildMergeChunksApplyOpsPrecond(const std::vector<ChunkType>& chunksToMerge,
                                          const ChunkVersion& collVersion) {
    BSONArrayBuilder preCond;

    for (auto chunk : chunksToMerge) {
        BSONObjBuilder b;
        b.append("ns", ChunkType::ConfigNS);
        b.append(
            "q",
            BSON("query" << BSON(ChunkType::ns(chunk.getNS()) << ChunkType::min(chunk.getMin())
                                                              << ChunkType::max(chunk.getMax()))
                         << "orderby"
                         << BSON(ChunkType::DEPRECATED_lastmod() << -1)));
        b.append("res",
                 BSON(ChunkType::DEPRECATED_epoch(collVersion.epoch())
                      << ChunkType::shard(chunk.getShard().toString())));
        preCond.append(b.obj());
    }
    return preCond.arr();
}

/**
 * Checks that the epoch in the version the shard sent with the command matches the epoch of the
 * collection version found on the config server. It is possible for a migration to end up running
 * partly without the protection of the distributed lock. This function checks that the collection
 * has not been dropped and recreated since the migration began, unbeknown to the shard when the
 * command was sent.
 */
Status checkCollectionVersionEpoch(OperationContext* txn,
                                   const NamespaceString& nss,
                                   const ChunkType& aChunk,
                                   const OID& collectionEpoch) {
    auto findResponseWith =
        Grid::get(txn)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            txn,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            NamespaceString(ChunkType::ConfigNS),
            BSON(ChunkType::ns() << nss.ns()),
            BSONObj(),
            1);
    if (!findResponseWith.isOK()) {
        return findResponseWith.getStatus();
    }

    if (MONGO_FAIL_POINT(migrationCommitVersionError)) {
        uassert(ErrorCodes::StaleEpoch,
                "failpoint 'migrationCommitVersionError' generated error",
                false);
    }

    if (findResponseWith.getValue().docs.empty()) {
        return Status(
            ErrorCodes::IncompatibleShardingMetadata,
            str::stream()
                << "Could not find any chunks for collection '"
                << nss.ns()
                << "'. The collection has been dropped since the migration began. Aborting"
                   " migration commit for chunk ("
                << redact(aChunk.getRange().toString())
                << ").");
    }

    auto chunkWith = ChunkType::fromBSON(findResponseWith.getValue().docs.front());
    if (!chunkWith.isOK()) {
        return chunkWith.getStatus();
    } else if (chunkWith.getValue().getVersion().epoch() != collectionEpoch) {
        return Status(ErrorCodes::StaleEpoch,
                      str::stream() << "The collection '" << nss.ns()
                                    << "' has been dropped and recreated since the migration began."
                                       " The config server's collection version epoch is now '"
                                    << chunkWith.getValue().getVersion().epoch().toString()
                                    << "', but the shard's is "
                                    << collectionEpoch.toString()
                                    << "'. Aborting migration commit for chunk ("
                                    << redact(aChunk.getRange().toString())
                                    << ").");
    }
    return Status::OK();
}

Status checkChunkIsOnShard(OperationContext* txn,
                           const NamespaceString& nss,
                           const BSONObj& min,
                           const BSONObj& max,
                           const ShardId& shard) {
    BSONObj chunkQuery =
        BSON(ChunkType::ns() << nss.ns() << ChunkType::min() << min << ChunkType::max() << max
                             << ChunkType::shard()
                             << shard);

    // Must use local read concern because we're going to perform subsequent writes.
    auto findResponseWith =
        Grid::get(txn)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            txn,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            NamespaceString(ChunkType::ConfigNS),
            chunkQuery,
            BSONObj(),
            1);
    if (!findResponseWith.isOK()) {
        return findResponseWith.getStatus();
    }

    if (findResponseWith.getValue().docs.empty()) {
        return {ErrorCodes::Error(40165),
                str::stream()
                    << "Could not find the chunk ("
                    << chunkQuery.toString()
                    << ") on the shard. Cannot execute the migration commit with invalid chunks."};
    }

    return Status::OK();
}

BSONObj makeCommitChunkApplyOpsCommand(const NamespaceString& nss,
                                       const ChunkType& migratedChunk,
                                       const boost::optional<ChunkType>& controlChunk,
                                       StringData fromShard,
                                       StringData toShard) {

    // Update migratedChunk's version and shard.
    BSONArrayBuilder updates;
    {
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);  // No upserting
        op.append("ns", ChunkType::ConfigNS);

        BSONObjBuilder n(op.subobjStart("o"));
        n.append(ChunkType::name(), ChunkType::genID(nss.ns(), migratedChunk.getMin()));
        migratedChunk.getVersion().addToBSON(n, ChunkType::DEPRECATED_lastmod());
        n.append(ChunkType::ns(), nss.ns());
        n.append(ChunkType::min(), migratedChunk.getMin());
        n.append(ChunkType::max(), migratedChunk.getMax());
        n.append(ChunkType::shard(), toShard);
        n.done();

        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), ChunkType::genID(nss.ns(), migratedChunk.getMin()));
        q.done();

        updates.append(op.obj());
    }

    // If we have a controlChunk, update its chunk version.
    if (controlChunk) {
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);
        op.append("ns", ChunkType::ConfigNS);

        BSONObjBuilder n(op.subobjStart("o"));
        n.append(ChunkType::name(), ChunkType::genID(nss.ns(), controlChunk->getMin()));
        controlChunk->getVersion().addToBSON(n, ChunkType::DEPRECATED_lastmod());
        n.append(ChunkType::ns(), nss.ns());
        n.append(ChunkType::min(), controlChunk->getMin());
        n.append(ChunkType::max(), controlChunk->getMax());
        n.append(ChunkType::shard(), fromShard);
        n.done();

        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), ChunkType::genID(nss.ns(), controlChunk->getMin()));
        q.done();

        updates.append(op.obj());
    }

    // Do not give applyOps a write concern. If applyOps tries to wait for replication, it will fail
    // because of the GlobalWrite lock CommitChunkMigration already holds. Replication will not be
    // able to take the lock it requires.
    return BSON("applyOps" << updates.arr());
}

}  // namespace

Status ShardingCatalogManagerImpl::commitChunkSplit(OperationContext* txn,
                                                    const NamespaceString& ns,
                                                    const OID& requestEpoch,
                                                    const ChunkRange& range,
                                                    const std::vector<BSONObj>& splitPoints,
                                                    const std::string& shardName) {
    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel
    Lock::ExclusiveLock lk(txn->lockState(), _kChunkOpLock);

    // Acquire GlobalLock in MODE_X twice to prevent yielding.
    // GlobalLock and the following lock on config.chunks are only needed to support
    // mixed-mode operation with mongoses from 3.2
    // TODO(SERVER-25337): Remove GlobalLock and config.chunks lock after 3.4
    Lock::GlobalLock firstGlobalLock(txn->lockState(), MODE_X, UINT_MAX);
    Lock::GlobalLock secondGlobalLock(txn->lockState(), MODE_X, UINT_MAX);

    // Acquire lock on config.chunks in MODE_X
    AutoGetCollection autoColl(txn, NamespaceString(ChunkType::ConfigNS), MODE_X);

    // Get the chunk with highest version for this namespace
    auto findStatus = Grid::get(txn)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        txn,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString(ChunkType::ConfigNS),
        BSON("ns" << ns.ns()),
        BSON(ChunkType::DEPRECATED_lastmod << -1),
        1);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& chunksVector = findStatus.getValue().docs;
    if (chunksVector.empty())
        return {ErrorCodes::IllegalOperation,
                "collection does not exist, isn't sharded, or has no chunks"};

    ChunkVersion collVersion =
        ChunkVersion::fromBSON(chunksVector.front(), ChunkType::DEPRECATED_lastmod());

    // Return an error if epoch of chunk does not match epoch of request
    if (collVersion.epoch() != requestEpoch) {
        return {ErrorCodes::StaleEpoch,
                "epoch of chunk does not match epoch of request. This most likely means "
                "that the collection was dropped and re-created."};
    }

    std::vector<ChunkType> newChunks;

    ChunkVersion currentMaxVersion = collVersion;

    auto startKey = range.getMin();
    auto newChunkBounds(splitPoints);
    newChunkBounds.push_back(range.getMax());

    BSONArrayBuilder updates;

    for (const auto& endKey : newChunkBounds) {
        // Verify the split points are all within the chunk
        if (endKey.woCompare(range.getMax()) != 0 && !range.containsKey(endKey)) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "Split key " << endKey << " not contained within chunk "
                                  << range.toString()};
        }

        // Verify the split points came in increasing order
        if (endKey.woCompare(startKey) < 0) {
            return {
                ErrorCodes::InvalidOptions,
                str::stream() << "Split keys must be specified in strictly increasing order. Key "
                              << endKey
                              << " was specified after "
                              << startKey
                              << "."};
        }

        // Verify that splitPoints are not repeated
        if (endKey.woCompare(startKey) == 0) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "Split on lower bound of chunk "
                                  << ChunkRange(startKey, endKey).toString()
                                  << "is not allowed"};
        }

        // verify that splits don't create too-big shard keys
        Status shardKeyStatus = ShardKeyPattern::checkShardKeySize(endKey);
        if (!shardKeyStatus.isOK()) {
            return shardKeyStatus;
        }

        // splits only update the 'minor' portion of version
        currentMaxVersion.incMinor();

        // build an update operation against the chunks collection of the config database
        // with upsert true
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", true);
        op.append("ns", ChunkType::ConfigNS);

        // add the modified (new) chunk information as the update object
        BSONObjBuilder n(op.subobjStart("o"));
        n.append(ChunkType::name(), ChunkType::genID(ns.ns(), startKey));
        currentMaxVersion.addToBSON(n, ChunkType::DEPRECATED_lastmod());
        n.append(ChunkType::ns(), ns.ns());
        n.append(ChunkType::min(), startKey);
        n.append(ChunkType::max(), endKey);
        n.append(ChunkType::shard(), shardName);
        n.done();

        // add the chunk's _id as the query part of the update statement
        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), ChunkType::genID(ns.ns(), startKey));
        q.done();

        updates.append(op.obj());

        // remember this chunk info for logging later
        ChunkType chunk;
        chunk.setMin(startKey);
        chunk.setMax(endKey);
        chunk.setVersion(currentMaxVersion);

        newChunks.push_back(std::move(chunk));

        startKey = endKey;
    }

    BSONArrayBuilder preCond;
    {
        BSONObjBuilder b;
        b.append("ns", ChunkType::ConfigNS);
        b.append("q",
                 BSON("query" << BSON(ChunkType::ns(ns.ns()) << ChunkType::min() << range.getMin()
                                                             << ChunkType::max()
                                                             << range.getMax())
                              << "orderby"
                              << BSON(ChunkType::DEPRECATED_lastmod() << -1)));
        {
            BSONObjBuilder bb(b.subobjStart("res"));
            bb.append(ChunkType::DEPRECATED_epoch(), requestEpoch);
            bb.append(ChunkType::shard(), shardName);
        }
        preCond.append(b.obj());
    }

    // apply the batch of updates to remote and local metadata
    Status applyOpsStatus = Grid::get(txn)->catalogClient(txn)->applyChunkOpsDeprecated(
        txn,
        updates.arr(),
        preCond.arr(),
        ns.ns(),
        currentMaxVersion,
        WriteConcernOptions(),
        repl::ReadConcernLevel::kLocalReadConcern);
    if (!applyOpsStatus.isOK()) {
        return applyOpsStatus;
    }

    // log changes
    BSONObjBuilder logDetail;
    {
        BSONObjBuilder b(logDetail.subobjStart("before"));
        b.append(ChunkType::min(), range.getMin());
        b.append(ChunkType::max(), range.getMax());
        collVersion.addToBSON(b, ChunkType::DEPRECATED_lastmod());
    }

    if (newChunks.size() == 2) {
        appendShortVersion(&logDetail.subobjStart("left"), newChunks[0]);
        appendShortVersion(&logDetail.subobjStart("right"), newChunks[1]);

        Grid::get(txn)->catalogClient(txn)->logChange(
            txn, "split", ns.ns(), logDetail.obj(), WriteConcernOptions());
    } else {
        BSONObj beforeDetailObj = logDetail.obj();
        BSONObj firstDetailObj = beforeDetailObj.getOwned();
        const int newChunksSize = newChunks.size();

        for (int i = 0; i < newChunksSize; i++) {
            BSONObjBuilder chunkDetail;
            chunkDetail.appendElements(beforeDetailObj);
            chunkDetail.append("number", i + 1);
            chunkDetail.append("of", newChunksSize);
            appendShortVersion(&chunkDetail.subobjStart("chunk"), newChunks[i]);

            Grid::get(txn)->catalogClient(txn)->logChange(
                txn, "multi-split", ns.ns(), chunkDetail.obj(), WriteConcernOptions());
        }
    }

    return applyOpsStatus;
}

Status ShardingCatalogManagerImpl::commitChunkMerge(OperationContext* txn,
                                                    const NamespaceString& ns,
                                                    const OID& requestEpoch,
                                                    const std::vector<BSONObj>& chunkBoundaries,
                                                    const std::string& shardName) {
    // This method must never be called with empty chunks to merge
    invariant(!chunkBoundaries.empty());

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel
    Lock::ExclusiveLock lk(txn->lockState(), _kChunkOpLock);

    // Acquire GlobalLock in MODE_X twice to prevent yielding.
    // GLobalLock and the following lock on config.chunks are only needed to support
    // mixed-mode operation with mongoses from 3.2
    // TODO(SERVER-25337): Remove GlobalLock and config.chunks lock after 3.4
    Lock::GlobalLock firstGlobalLock(txn->lockState(), MODE_X, UINT_MAX);
    Lock::GlobalLock secondGlobalLock(txn->lockState(), MODE_X, UINT_MAX);

    // Acquire lock on config.chunks in MODE_X
    AutoGetCollection autoColl(txn, NamespaceString(ChunkType::ConfigNS), MODE_X);

    // Get the chunk with the highest version for this namespace
    auto findStatus = Grid::get(txn)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        txn,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString(ChunkType::ConfigNS),
        BSON("ns" << ns.ns()),
        BSON(ChunkType::DEPRECATED_lastmod << -1),
        1);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& chunksVector = findStatus.getValue().docs;
    if (chunksVector.empty())
        return {ErrorCodes::IllegalOperation,
                "collection does not exist, isn't sharded, or has no chunks"};

    ChunkVersion collVersion =
        ChunkVersion::fromBSON(chunksVector.front(), ChunkType::DEPRECATED_lastmod());

    // Return an error if epoch of chunk does not match epoch of request
    if (collVersion.epoch() != requestEpoch) {
        return {ErrorCodes::StaleEpoch,
                "epoch of chunk does not match epoch of request. This most likely means "
                "that the collection was dropped and re-created."};
    }

    // Build chunks to be merged
    std::vector<ChunkType> chunksToMerge;

    ChunkType itChunk;
    itChunk.setMax(chunkBoundaries.front());
    itChunk.setNS(ns.ns());
    itChunk.setShard(shardName);

    // Do not use the first chunk boundary as a max bound while building chunks
    for (size_t i = 1; i < chunkBoundaries.size(); ++i) {
        itChunk.setMin(itChunk.getMax());

        // Ensure the chunk boundaries are strictly increasing
        if (chunkBoundaries[i].woCompare(itChunk.getMin()) <= 0) {
            return {
                ErrorCodes::InvalidOptions,
                str::stream()
                    << "Chunk boundaries must be specified in strictly increasing order. Boundary "
                    << chunkBoundaries[i]
                    << " was specified after "
                    << itChunk.getMin()
                    << "."};
        }

        itChunk.setMax(chunkBoundaries[i]);
        chunksToMerge.push_back(itChunk);
    }

    ChunkVersion mergeVersion = collVersion;
    mergeVersion.incMinor();

    auto updates = buildMergeChunksApplyOpsUpdates(chunksToMerge, mergeVersion);
    auto preCond = buildMergeChunksApplyOpsPrecond(chunksToMerge, collVersion);

    // apply the batch of updates to remote and local metadata
    Status applyOpsStatus = Grid::get(txn)->catalogClient(txn)->applyChunkOpsDeprecated(
        txn,
        updates,
        preCond,
        ns.ns(),
        mergeVersion,
        WriteConcernOptions(),
        repl::ReadConcernLevel::kLocalReadConcern);
    if (!applyOpsStatus.isOK()) {
        return applyOpsStatus;
    }

    // log changes
    BSONObjBuilder logDetail;
    {
        BSONArrayBuilder b(logDetail.subarrayStart("merged"));
        for (auto chunkToMerge : chunksToMerge) {
            b.append(chunkToMerge.toBSON());
        }
    }
    collVersion.addToBSON(logDetail, "prevShardVersion");
    mergeVersion.addToBSON(logDetail, "mergedVersion");

    Grid::get(txn)->catalogClient(txn)->logChange(
        txn, "merge", ns.ns(), logDetail.obj(), WriteConcernOptions());

    return applyOpsStatus;
}

StatusWith<BSONObj> ShardingCatalogManagerImpl::commitChunkMigration(
    OperationContext* txn,
    const NamespaceString& nss,
    const ChunkType& migratedChunk,
    const boost::optional<ChunkType>& controlChunk,
    const OID& collectionEpoch,
    const ShardId& fromShard,
    const ShardId& toShard) {

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations.
    //
    // ConfigSvrCommitChunkMigration commands must be run serially because the new ChunkVersions
    // for migrated chunks are generated within the command and must be committed to the database
    // before another chunk commit generates new ChunkVersions in the same manner.
    //
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel.
    // (Note: This is not needed while we have a global lock, taken here only for consistency.)
    Lock::ExclusiveLock lk(txn->lockState(), _kChunkOpLock);

    // Acquire GlobalLock in MODE_X twice to prevent yielding.
    // Run operations under a nested lock as a hack to prevent yielding. When query/applyOps
    // commands are called, they will take a second lock, and the PlanExecutor will be unable to
    // yield.
    //
    // ConfigSvrCommitChunkMigration commands must be run serially because the new ChunkVersions
    // for migrated chunks are generated within the command. Therefore it cannot be allowed to
    // yield between generating the ChunkVersion and committing it to the database with
    // applyOps.

    Lock::GlobalWrite firstGlobalWriteLock(txn->lockState());

    // Ensure that the epoch passed in still matches the real state of the database.

    auto epochCheck = checkCollectionVersionEpoch(txn, nss, migratedChunk, collectionEpoch);
    if (!epochCheck.isOK()) {
        return epochCheck;
    }

    // Check that migratedChunk and controlChunk are where they should be, on fromShard.

    auto migratedOnShard =
        checkChunkIsOnShard(txn, nss, migratedChunk.getMin(), migratedChunk.getMax(), fromShard);
    if (!migratedOnShard.isOK()) {
        return migratedOnShard;
    }

    if (controlChunk) {
        auto controlOnShard = checkChunkIsOnShard(
            txn, nss, controlChunk->getMin(), controlChunk->getMax(), fromShard);
        if (!controlOnShard.isOK()) {
            return controlOnShard;
        }
    }

    // Must use local read concern because we will perform subsequent writes.
    auto findResponse = Grid::get(txn)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        txn,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString(ChunkType::ConfigNS),
        BSON("ns" << nss.ns()),
        BSON(ChunkType::DEPRECATED_lastmod << -1),
        1);
    if (!findResponse.isOK()) {
        return findResponse.getStatus();
    }

    std::vector<BSONObj> chunksVector = std::move(findResponse.getValue().docs);
    if (chunksVector.empty()) {
        return Status(ErrorCodes::Error(40164),
                      str::stream() << "Tried to find max chunk version for collection '"
                                    << nss.ns()
                                    << ", but found no chunks");
    }

    ChunkVersion currentMaxVersion =
        ChunkVersion::fromBSON(chunksVector.front(), ChunkType::DEPRECATED_lastmod());

    // Use the incremented major version of the result returned.

    // Generate the new versions of migratedChunk and controlChunk.
    // Migrating chunk's minor version will be 0.
    ChunkType newMigratedChunk = migratedChunk;
    newMigratedChunk.setVersion(
        ChunkVersion(currentMaxVersion.majorVersion() + 1, 0, currentMaxVersion.epoch()));

    // Control chunk's minor version will be 1 (if control chunk is present).
    boost::optional<ChunkType> newControlChunk = boost::none;
    if (controlChunk) {
        newControlChunk = controlChunk.get();
        newControlChunk->setVersion(
            ChunkVersion(currentMaxVersion.majorVersion() + 1, 1, currentMaxVersion.epoch()));
    }

    auto command = makeCommitChunkApplyOpsCommand(
        nss, newMigratedChunk, newControlChunk, fromShard.toString(), toShard.toString());

    StatusWith<Shard::CommandResponse> applyOpsCommandResponse =
        Grid::get(txn)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            txn,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            nss.db().toString(),
            command,
            Shard::RetryPolicy::kIdempotent);

    if (!applyOpsCommandResponse.isOK()) {
        return applyOpsCommandResponse.getStatus();
    }
    if (!applyOpsCommandResponse.getValue().commandStatus.isOK()) {
        return applyOpsCommandResponse.getValue().commandStatus;
    }

    BSONObjBuilder result;
    newMigratedChunk.getVersion().appendWithFieldForCommands(&result, "migratedChunkVersion");
    if (controlChunk) {
        newControlChunk->getVersion().appendWithFieldForCommands(&result, "controlChunkVersion");
    }
    return result.obj();
}

}  // namespace mongo
