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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog_cache.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

// How many times to try refreshing the routing info if the set of chunks loaded from the config
// server is found to be inconsistent.
const int kMaxInconsistentRoutingInfoRefreshAttempts = 3;

/**
 * Given an (optional) initial routing table and a set of changed chunks returned by the catalog
 * cache loader, produces a new routing table with the changes applied.
 *
 * If the collection is no longer sharded returns nullptr. If the epoch has changed, expects that
 * the 'collectionChunksList' contains the full contents of the chunks collection for that namespace
 * so that the routing table can be built from scratch.
 *
 * Throws ConflictingOperationInProgress if the chunk metadata was found to be inconsistent (not
 * containing all the necessary chunks, contains overlaps or chunks' epoch values are not the same
 * as that of the collection). Since this situation may be transient, due to the collection being
 * dropped or recreated concurrently, the caller must retry the reload up to some configurable
 * number of attempts.
 */
std::shared_ptr<ChunkManager> refreshCollectionRoutingInfo(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::shared_ptr<ChunkManager> existingRoutingInfo,
    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollectionAndChangedChunks) {
    if (swCollectionAndChangedChunks == ErrorCodes::NamespaceNotFound) {
        return nullptr;
    }

    const auto collectionAndChunks = uassertStatusOK(std::move(swCollectionAndChangedChunks));

    // Check whether the collection epoch might have changed
    ChunkVersion startingCollectionVersion;
    ChunkMap chunkMap =
        SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<std::shared_ptr<Chunk>>();

    if (!existingRoutingInfo) {
        // If we don't have a basis chunk manager, do a full refresh
        startingCollectionVersion = ChunkVersion(0, 0, collectionAndChunks.epoch);
    } else if (existingRoutingInfo->getVersion().epoch() != collectionAndChunks.epoch) {
        // If the collection's epoch has changed, do a full refresh
        startingCollectionVersion = ChunkVersion(0, 0, collectionAndChunks.epoch);
    } else {
        startingCollectionVersion = existingRoutingInfo->getVersion();
        chunkMap = existingRoutingInfo->chunkMap();
    }

    ChunkVersion collectionVersion = startingCollectionVersion;

    for (const auto& chunk : collectionAndChunks.changedChunks) {
        const auto& chunkVersion = chunk.getVersion();

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Chunk " << chunk.genID(nss.ns(), chunk.getMin())
                              << " has epoch different from that of the collection "
                              << chunkVersion.epoch(),
                collectionVersion.epoch() == chunkVersion.epoch());

        // Chunks must always come in incrementally sorted order
        invariant(chunkVersion >= collectionVersion);
        collectionVersion = chunkVersion;

        // Ensure chunk references a valid shard and that the shard is available and loaded
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, chunk.getShard()));

        // Returns the first chunk with a max key that is > min - implies that the chunk overlaps
        // min
        const auto low = chunkMap.upper_bound(chunk.getMin());

        // Returns the first chunk with a max key that is > max - implies that the next chunk cannot
        // not overlap max
        const auto high = chunkMap.upper_bound(chunk.getMax());

        // Erase all chunks from the map, which overlap the chunk we got from the persistent store
        chunkMap.erase(low, high);

        // Insert only the chunk itself
        chunkMap.insert(std::make_pair(chunk.getMax(), std::make_shared<Chunk>(chunk)));
    }

    // If at least one diff was applied, the metadata is correct, but it might not have changed so
    // in this case there is no need to recreate the chunk manager.
    //
    // NOTE: In addition to the above statement, it is also important that we return the same chunk
    // manager object, because the write commands' code relies on changes of the chunk manager's
    // sequence number to detect batch writes not making progress because of chunks moving across
    // shards too frequently.
    if (collectionVersion == startingCollectionVersion) {
        return existingRoutingInfo;
    }

    std::unique_ptr<CollatorInterface> defaultCollator;
    if (!collectionAndChunks.defaultCollation.isEmpty()) {
        // The collation should have been validated upon collection creation
        defaultCollator = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(collectionAndChunks.defaultCollation));
    }

    return stdx::make_unique<ChunkManager>(nss,
                                           KeyPattern(collectionAndChunks.shardKeyPattern),
                                           std::move(defaultCollator),
                                           collectionAndChunks.shardKeyIsUnique,
                                           std::move(chunkMap),
                                           collectionVersion);
}

}  // namespace

CatalogCache::CatalogCache() : _cacheLoader(stdx::make_unique<ConfigServerCatalogCacheLoader>()) {}

CatalogCache::~CatalogCache() = default;

StatusWith<CachedDatabaseInfo> CatalogCache::getDatabase(OperationContext* opCtx,
                                                         StringData dbName) {
    try {
        return {CachedDatabaseInfo(_getDatabase(opCtx, dbName))};
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss) {
    while (true) {
        std::shared_ptr<DatabaseInfoEntry> dbEntry;
        try {
            dbEntry = _getDatabase(opCtx, nss.db());
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        stdx::unique_lock<stdx::mutex> ul(_mutex);

        auto& collections = dbEntry->collections;

        auto it = collections.find(nss.ns());
        if (it == collections.end()) {
            auto shardStatus =
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dbEntry->primaryShardId);
            if (!shardStatus.isOK()) {
                return {ErrorCodes::fromInt(40371),
                        str::stream() << "The primary shard for collection " << nss.ns()
                                      << " could not be loaded due to error "
                                      << shardStatus.getStatus().toString()};
            }

            return {CachedCollectionRoutingInfo(
                dbEntry->primaryShardId, nss, std::move(shardStatus.getValue()))};
        }

        auto& collEntry = it->second;

        if (collEntry.needsRefresh) {
            auto refreshNotification = collEntry.refreshCompletionNotification;
            if (!refreshNotification) {
                refreshNotification = (collEntry.refreshCompletionNotification =
                                           std::make_shared<Notification<Status>>());
                _scheduleCollectionRefresh_inlock(
                    dbEntry, std::move(collEntry.routingInfo), nss, 1);
            }

            // Wait on the notification outside of the mutex
            ul.unlock();

            auto refreshStatus = [&]() {
                try {
                    return refreshNotification->get(opCtx);
                } catch (const DBException& ex) {
                    return ex.toStatus();
                }
            }();

            if (!refreshStatus.isOK()) {
                return refreshStatus;
            }

            // Once the refresh is complete, loop around to get the latest value
            continue;
        }

        return {CachedCollectionRoutingInfo(dbEntry->primaryShardId, collEntry.routingInfo)};
    }
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfo(
    OperationContext* opCtx, StringData ns) {
    return getCollectionRoutingInfo(opCtx, NamespaceString(ns));
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getShardedCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    invalidateShardedCollection(nss);

    auto routingInfoStatus = getCollectionRoutingInfo(opCtx, nss);
    if (routingInfoStatus.isOK() && !routingInfoStatus.getValue().cm()) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " is not sharded."};
    }

    return routingInfoStatus;
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getShardedCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, StringData ns) {
    return getShardedCollectionRoutingInfoWithRefresh(opCtx, NamespaceString(ns));
}

void CatalogCache::onStaleConfigError(CachedCollectionRoutingInfo&& ccriToInvalidate) {
    // Ensure the move constructor of CachedCollectionRoutingInfo is invoked in order to clear the
    // input argument so it can't be used anymore
    auto ccri(ccriToInvalidate);

    if (!ccri._cm) {
        // Here we received a stale config error for a collection which we previously thought was
        // unsharded.
        invalidateShardedCollection(ccri._nss);
        return;
    }

    // Here we received a stale config error for a collection which we previously though was sharded
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _databases.find(NamespaceString(ccri._cm->getns()).db());
    if (it == _databases.end()) {
        // If the database does not exist, the collection must have been dropped so there is
        // nothing to invalidate. The getCollectionRoutingInfo will handle the reload of the
        // entire database and its collections.
        return;
    }

    auto& collections = it->second->collections;

    auto itColl = collections.find(ccri._cm->getns());
    if (itColl == collections.end()) {
        // If the collection does not exist, this means it must have been dropped since the last
        // time we retrieved a cache entry for it. Doing nothing in this case will cause the
        // next call to getCollectionRoutingInfo to return an unsharded collection.
        return;
    } else if (itColl->second.needsRefresh) {
        // Refresh has been scheduled for the collection already
        return;
    } else if (itColl->second.routingInfo->getVersion() == ccri._cm->getVersion()) {
        // If the versions match, the last version of the routing information that we used is no
        // longer valid, so trigger a refresh.
        itColl->second.needsRefresh = true;
    }
}

void CatalogCache::invalidateShardedCollection(const NamespaceString& nss) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _databases.find(nss.db());
    if (it == _databases.end()) {
        return;
    }

    it->second->collections[nss.ns()].needsRefresh = true;
}

void CatalogCache::invalidateShardedCollection(StringData ns) {
    invalidateShardedCollection(NamespaceString(ns));
}

void CatalogCache::purgeDatabase(StringData dbName) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _databases.find(dbName);
    if (it == _databases.end()) {
        return;
    }

    _databases.erase(it);
}

void CatalogCache::purgeAllDatabases() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _databases.clear();
}

std::shared_ptr<CatalogCache::DatabaseInfoEntry> CatalogCache::_getDatabase(OperationContext* opCtx,
                                                                            StringData dbName) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _databases.find(dbName);
    if (it != _databases.end()) {
        return it->second;
    }

    const auto catalogClient = Grid::get(opCtx)->catalogClient(opCtx);

    const auto dbNameCopy = dbName.toString();

    // Load the database entry
    const auto opTimeWithDb = uassertStatusOK(catalogClient->getDatabase(opCtx, dbNameCopy));
    const auto& dbDesc = opTimeWithDb.value;

    // Load the sharded collections entries
    std::vector<CollectionType> collections;
    repl::OpTime collLoadConfigOptime;
    uassertStatusOK(
        catalogClient->getCollections(opCtx, &dbNameCopy, &collections, &collLoadConfigOptime));

    StringMap<CollectionRoutingInfoEntry> collectionEntries;
    for (const auto& coll : collections) {
        if (coll.getDropped()) {
            continue;
        }

        collectionEntries[coll.getNs().ns()].needsRefresh = true;
    }

    return _databases[dbName] = std::shared_ptr<DatabaseInfoEntry>(new DatabaseInfoEntry{
               dbDesc.getPrimary(), dbDesc.getSharded(), std::move(collectionEntries)});
}

void CatalogCache::_scheduleCollectionRefresh_inlock(
    std::shared_ptr<DatabaseInfoEntry> dbEntry,
    std::shared_ptr<ChunkManager> existingRoutingInfo,
    const NamespaceString& nss,
    int refreshAttempt) {
    Timer t;

    const ChunkVersion startingCollectionVersion =
        (existingRoutingInfo ? existingRoutingInfo->getVersion() : ChunkVersion::UNSHARDED());

    const auto refreshFailed_inlock =
        [ this, t, dbEntry, nss, refreshAttempt ](const Status& status) noexcept {
        log() << "Refresh for collection " << nss << " took " << t.millis() << " ms and failed"
              << causedBy(redact(status));

        auto& collections = dbEntry->collections;
        auto it = collections.find(nss.ns());
        invariant(it != collections.end());
        auto& collEntry = it->second;

        // It is possible that the metadata is being changed concurrently, so retry the
        // refresh again
        if (status == ErrorCodes::ConflictingOperationInProgress &&
            refreshAttempt < kMaxInconsistentRoutingInfoRefreshAttempts) {
            _scheduleCollectionRefresh_inlock(dbEntry, nullptr, nss, refreshAttempt + 1);
        } else {
            // Leave needsRefresh to true so that any subsequent get attempts will kick off
            // another round of refresh
            collEntry.refreshCompletionNotification->set(status);
            collEntry.refreshCompletionNotification = nullptr;
        }
    };

    const auto refreshCallback =
        [ this, t, dbEntry, nss, existingRoutingInfo, refreshFailed_inlock ](
            OperationContext * opCtx,
            StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        std::shared_ptr<ChunkManager> newRoutingInfo;
        try {
            newRoutingInfo = refreshCollectionRoutingInfo(
                opCtx, nss, std::move(existingRoutingInfo), std::move(swCollAndChunks));
        } catch (const DBException& ex) {
            stdx::lock_guard<stdx::mutex> lg(_mutex);
            refreshFailed_inlock(ex.toStatus());
            return;
        }

        stdx::lock_guard<stdx::mutex> lg(_mutex);
        auto& collections = dbEntry->collections;
        auto it = collections.find(nss.ns());
        invariant(it != collections.end());
        auto& collEntry = it->second;

        collEntry.needsRefresh = false;
        collEntry.refreshCompletionNotification->set(Status::OK());
        collEntry.refreshCompletionNotification = nullptr;

        if (!newRoutingInfo) {
            log() << "Refresh for collection " << nss << " took " << t.millis()
                  << " and found the collection is not sharded";

            collections.erase(it);
        } else {
            log() << "Refresh for collection " << nss << " took " << t.millis()
                  << " ms and found version " << newRoutingInfo->getVersion();

            collEntry.routingInfo = std::move(newRoutingInfo);
        }
    };

    log() << "Refreshing chunks for collection " << nss << " based on version "
          << startingCollectionVersion;

    try {
        _cacheLoader->getChunksSince(nss, startingCollectionVersion, refreshCallback);
    } catch (const DBException& ex) {
        const auto status = ex.toStatus();

        // ConflictingOperationInProgress errors trigger retry of the catalog cache reload logic. If
        // we failed to schedule the asynchronous reload, there is no point in doing another
        // attempt.
        invariant(status != ErrorCodes::ConflictingOperationInProgress);

        stdx::lock_guard<stdx::mutex> lg(_mutex);
        refreshFailed_inlock(status);
    }
}

CachedDatabaseInfo::CachedDatabaseInfo(std::shared_ptr<CatalogCache::DatabaseInfoEntry> db)
    : _db(std::move(db)) {}

const ShardId& CachedDatabaseInfo::primaryId() const {
    return _db->primaryShardId;
}

bool CachedDatabaseInfo::shardingEnabled() const {
    return _db->shardingEnabled;
}

CachedCollectionRoutingInfo::CachedCollectionRoutingInfo(ShardId primaryId,
                                                         std::shared_ptr<ChunkManager> cm)
    : _primaryId(std::move(primaryId)), _cm(std::move(cm)) {}

CachedCollectionRoutingInfo::CachedCollectionRoutingInfo(ShardId primaryId,
                                                         NamespaceString nss,
                                                         std::shared_ptr<Shard> primary)
    : _primaryId(std::move(primaryId)), _nss(std::move(nss)), _primary(std::move(primary)) {}

}  // namespace mongo
