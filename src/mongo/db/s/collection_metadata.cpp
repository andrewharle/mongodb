/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_metadata.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;
using std::make_pair;
using std::string;
using std::vector;
using str::stream;

CollectionMetadata::CollectionMetadata()
    : _pendingMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>()),
      _chunksMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>()),
      _rangesMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>()) {}

CollectionMetadata::CollectionMetadata(const BSONObj& keyPattern, ChunkVersion collectionVersion)
    : _collVersion(collectionVersion),
      _shardVersion(ChunkVersion(0, 0, collectionVersion.epoch())),
      _keyPattern(keyPattern.getOwned()),
      _pendingMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>()),
      _chunksMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>()),
      _rangesMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>()) {}

CollectionMetadata::~CollectionMetadata() = default;

unique_ptr<CollectionMetadata> CollectionMetadata::clonePlusChunk(
    const BSONObj& minKey, const BSONObj& maxKey, const ChunkVersion& chunkVersion) const {
    invariant(chunkVersion.epoch() == _shardVersion.epoch());
    invariant(chunkVersion.isSet());
    invariant(chunkVersion > _shardVersion);
    invariant(minKey.woCompare(maxKey) < 0);
    invariant(!rangeMapOverlaps(_chunksMap, minKey, maxKey));

    unique_ptr<CollectionMetadata> metadata(stdx::make_unique<CollectionMetadata>());
    metadata->_keyPattern = _keyPattern.getOwned();
    metadata->fillKeyPatternFields();
    metadata->_pendingMap = _pendingMap;
    metadata->_chunksMap = _chunksMap;
    metadata->_chunksMap.insert(
        make_pair(minKey.getOwned(), CachedChunkInfo(maxKey.getOwned(), chunkVersion)));
    metadata->_shardVersion = chunkVersion;
    metadata->_collVersion = chunkVersion > _collVersion ? chunkVersion : _collVersion;
    metadata->fillRanges();

    invariant(metadata->isValid());
    return metadata;
}

std::unique_ptr<CollectionMetadata> CollectionMetadata::cloneMinusPending(
    const ChunkType& chunk) const {
    invariant(rangeMapContains(_pendingMap, chunk.getMin(), chunk.getMax()));

    unique_ptr<CollectionMetadata> metadata(stdx::make_unique<CollectionMetadata>());
    metadata->_keyPattern = _keyPattern.getOwned();
    metadata->fillKeyPatternFields();
    metadata->_pendingMap = _pendingMap;
    metadata->_pendingMap.erase(chunk.getMin());

    metadata->_chunksMap = _chunksMap;
    metadata->_rangesMap = _rangesMap;
    metadata->_shardVersion = _shardVersion;
    metadata->_collVersion = _collVersion;

    invariant(metadata->isValid());
    return metadata;
}

std::unique_ptr<CollectionMetadata> CollectionMetadata::clonePlusPending(
    const ChunkType& chunk) const {
    invariant(!rangeMapOverlaps(_chunksMap, chunk.getMin(), chunk.getMax()));

    unique_ptr<CollectionMetadata> metadata(stdx::make_unique<CollectionMetadata>());
    metadata->_keyPattern = _keyPattern.getOwned();
    metadata->fillKeyPatternFields();
    metadata->_pendingMap = _pendingMap;
    metadata->_chunksMap = _chunksMap;
    metadata->_rangesMap = _rangesMap;
    metadata->_shardVersion = _shardVersion;
    metadata->_collVersion = _collVersion;

    // If there are any pending chunks on the interval to be added this is ok, since pending chunks
    // aren't officially tracked yet and something may have changed on servers we do not see yet.
    //
    // We remove any chunks we overlap because the remote request starting a chunk migration is what
    // is authoritative.

    if (rangeMapOverlaps(_pendingMap, chunk.getMin(), chunk.getMax())) {
        RangeVector pendingOverlap;
        getRangeMapOverlap(_pendingMap, chunk.getMin(), chunk.getMax(), &pendingOverlap);

        warning() << "new pending chunk " << redact(rangeToString(chunk.getMin(), chunk.getMax()))
                  << " overlaps existing pending chunks " << redact(overlapToString(pendingOverlap))
                  << ", a migration may not have completed";

        for (RangeVector::iterator it = pendingOverlap.begin(); it != pendingOverlap.end(); ++it) {
            metadata->_pendingMap.erase(it->first);
        }
    }

    // The pending map entry cannot contain a specific chunk version because we don't know what
    // version would be generated for it at commit time. That's why we insert an IGNORED value.
    metadata->_pendingMap.insert(
        make_pair(chunk.getMin(), CachedChunkInfo(chunk.getMax(), ChunkVersion::IGNORED())));

    invariant(metadata->isValid());
    return metadata;
}

bool CollectionMetadata::keyBelongsToMe(const BSONObj& key) const {
    // For now, collections don't move. So if the collection is not sharded, assume
    // the document with the given key can be accessed.
    if (_keyPattern.isEmpty()) {
        return true;
    }

    if (_rangesMap.size() <= 0) {
        return false;
    }

    RangeMap::const_iterator it = _rangesMap.upper_bound(key);
    if (it != _rangesMap.begin())
        it--;

    return rangeContains(it->first, it->second.getMaxKey(), key);
}

bool CollectionMetadata::keyIsPending(const BSONObj& key) const {
    // If we aren't sharded, then the key is never pending (though it belongs-to-me)
    if (_keyPattern.isEmpty()) {
        return false;
    }

    if (_pendingMap.size() <= 0) {
        return false;
    }

    RangeMap::const_iterator it = _pendingMap.upper_bound(key);
    if (it != _pendingMap.begin())
        it--;

    bool isPending = rangeContains(it->first, it->second.getMaxKey(), key);
    return isPending;
}

bool CollectionMetadata::getNextChunk(const BSONObj& lookupKey, ChunkType* chunk) const {
    RangeMap::const_iterator upperChunkIt = _chunksMap.upper_bound(lookupKey);
    RangeMap::const_iterator lowerChunkIt = upperChunkIt;

    if (upperChunkIt != _chunksMap.begin()) {
        --lowerChunkIt;
    } else {
        lowerChunkIt = _chunksMap.end();
    }

    if (lowerChunkIt != _chunksMap.end() &&
        lowerChunkIt->second.getMaxKey().woCompare(lookupKey) > 0) {
        chunk->setMin(lowerChunkIt->first);
        chunk->setMax(lowerChunkIt->second.getMaxKey());
        chunk->setVersion(lowerChunkIt->second.getVersion());
        return true;
    }

    if (upperChunkIt != _chunksMap.end()) {
        chunk->setMin(upperChunkIt->first);
        chunk->setMax(upperChunkIt->second.getMaxKey());
        chunk->setVersion(upperChunkIt->second.getVersion());
        return true;
    }

    return false;
}

bool CollectionMetadata::getDifferentChunk(const BSONObj& chunkMinKey,
                                           ChunkType* differentChunk) const {
    RangeMap::const_iterator upperChunkIt = _chunksMap.end();
    RangeMap::const_iterator lowerChunkIt = _chunksMap.begin();

    while (lowerChunkIt != upperChunkIt) {
        if (lowerChunkIt->first.woCompare(chunkMinKey) != 0) {
            differentChunk->setMin(lowerChunkIt->first);
            differentChunk->setMax(lowerChunkIt->second.getMaxKey());
            differentChunk->setVersion(lowerChunkIt->second.getVersion());
            return true;
        }
        ++lowerChunkIt;
    }

    return false;
}

Status CollectionMetadata::checkChunkIsValid(const ChunkType& chunk) {
    ChunkType existingChunk;

    if (!getNextChunk(chunk.getMin(), &existingChunk)) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "Chunk with bounds "
                              << ChunkRange(chunk.getMin(), chunk.getMax()).toString()
                              << " is not owned by this shard."};
    }

    if (existingChunk.getMin().woCompare(chunk.getMin()) ||
        existingChunk.getMax().woCompare(chunk.getMax())) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "Unable to find chunk with the exact bounds "
                              << ChunkRange(chunk.getMin(), chunk.getMax()).toString()
                              << " at collection version "
                              << getCollVersion().toString()};
    }

    if (chunk.isVersionSet() && !chunk.getVersion().isStrictlyEqualTo(existingChunk.getVersion())) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "Chunk with the specified bounds exists but the version does not "
                                 "match. Expected: "
                              << chunk.getVersion().toString()
                              << ", actual: "
                              << existingChunk.getVersion().toString()};
    }

    return Status::OK();
}

void CollectionMetadata::toBSONBasic(BSONObjBuilder& bb) const {
    _collVersion.addToBSON(bb, "collVersion");
    _shardVersion.addToBSON(bb, "shardVersion");
    bb.append("keyPattern", _keyPattern);
}

void CollectionMetadata::toBSONChunks(BSONArrayBuilder& bb) const {
    if (_chunksMap.empty())
        return;

    for (RangeMap::const_iterator it = _chunksMap.begin(); it != _chunksMap.end(); ++it) {
        BSONArrayBuilder chunkBB(bb.subarrayStart());
        chunkBB.append(it->first);
        chunkBB.append(it->second.getMaxKey());
        chunkBB.done();
    }
}

void CollectionMetadata::toBSONPending(BSONArrayBuilder& bb) const {
    if (_pendingMap.empty())
        return;

    for (RangeMap::const_iterator it = _pendingMap.begin(); it != _pendingMap.end(); ++it) {
        BSONArrayBuilder pendingBB(bb.subarrayStart());
        pendingBB.append(it->first);
        pendingBB.append(it->second.getMaxKey());
        pendingBB.done();
    }
}

string CollectionMetadata::toStringBasic() const {
    return stream() << "collection version: " << _collVersion.toString()
                    << ", shard version: " << _shardVersion.toString();
}

bool CollectionMetadata::getNextOrphanRange(const BSONObj& origLookupKey, KeyRange* range) const {
    if (_keyPattern.isEmpty())
        return false;

    BSONObj lookupKey = origLookupKey;
    BSONObj maxKey = getMaxKey();  // so we don't keep rebuilding
    while (lookupKey.woCompare(maxKey) < 0) {
        RangeMap::const_iterator lowerChunkIt = _chunksMap.end();
        RangeMap::const_iterator upperChunkIt = _chunksMap.end();

        if (!_chunksMap.empty()) {
            upperChunkIt = _chunksMap.upper_bound(lookupKey);
            lowerChunkIt = upperChunkIt;
            if (upperChunkIt != _chunksMap.begin())
                --lowerChunkIt;
            else
                lowerChunkIt = _chunksMap.end();
        }

        // If we overlap, continue after the overlap
        // TODO: Could optimize slightly by finding next non-contiguous chunk
        if (lowerChunkIt != _chunksMap.end() &&
            lowerChunkIt->second.getMaxKey().woCompare(lookupKey) > 0) {
            lookupKey = lowerChunkIt->second.getMaxKey();
            continue;
        }

        RangeMap::const_iterator lowerPendingIt = _pendingMap.end();
        RangeMap::const_iterator upperPendingIt = _pendingMap.end();

        if (!_pendingMap.empty()) {
            upperPendingIt = _pendingMap.upper_bound(lookupKey);
            lowerPendingIt = upperPendingIt;
            if (upperPendingIt != _pendingMap.begin())
                --lowerPendingIt;
            else
                lowerPendingIt = _pendingMap.end();
        }

        // If we overlap, continue after the overlap
        // TODO: Could optimize slightly by finding next non-contiguous chunk
        if (lowerPendingIt != _pendingMap.end() &&
            lowerPendingIt->second.getMaxKey().woCompare(lookupKey) > 0) {
            lookupKey = lowerPendingIt->second.getMaxKey();
            continue;
        }

        //
        // We know that the lookup key is not covered by a chunk or pending range, and where the
        // previous chunk and pending chunks are.  Now we fill in the bounds as the closest
        // bounds of the surrounding ranges in both maps.
        //

        range->keyPattern = _keyPattern;
        range->minKey = getMinKey();
        range->maxKey = maxKey;

        if (lowerChunkIt != _chunksMap.end() &&
            lowerChunkIt->second.getMaxKey().woCompare(range->minKey) > 0) {
            range->minKey = lowerChunkIt->second.getMaxKey();
        }

        if (upperChunkIt != _chunksMap.end() && upperChunkIt->first.woCompare(range->maxKey) < 0) {
            range->maxKey = upperChunkIt->first;
        }

        if (lowerPendingIt != _pendingMap.end() &&
            lowerPendingIt->second.getMaxKey().woCompare(range->minKey) > 0) {
            range->minKey = lowerPendingIt->second.getMaxKey();
        }

        if (upperPendingIt != _pendingMap.end() &&
            upperPendingIt->first.woCompare(range->maxKey) < 0) {
            range->maxKey = upperPendingIt->first;
        }

        return true;
    }

    return false;
}

BSONObj CollectionMetadata::getMinKey() const {
    BSONObjIterator it(_keyPattern);
    BSONObjBuilder minKeyB;
    while (it.more())
        minKeyB << it.next().fieldName() << MINKEY;
    return minKeyB.obj();
}

BSONObj CollectionMetadata::getMaxKey() const {
    BSONObjIterator it(_keyPattern);
    BSONObjBuilder maxKeyB;
    while (it.more())
        maxKeyB << it.next().fieldName() << MAXKEY;
    return maxKeyB.obj();
}

bool CollectionMetadata::isValid() const {
    if (_shardVersion > _collVersion)
        return false;
    if (_collVersion.majorVersion() == 0)
        return false;
    if (_collVersion.epoch() != _shardVersion.epoch())
        return false;

    if (_shardVersion.majorVersion() > 0) {
        // Must be chunks
        if (_rangesMap.size() == 0 || _chunksMap.size() == 0)
            return false;
    } else {
        // No chunks
        if (_shardVersion.minorVersion() > 0)
            return false;
        if (_rangesMap.size() > 0 || _chunksMap.size() > 0)
            return false;
    }

    return true;
}

bool CollectionMetadata::isValidKey(const BSONObj& key) const {
    BSONObjIterator it(_keyPattern);
    while (it.more()) {
        BSONElement next = it.next();
        if (!key.hasField(next.fieldName()))
            return false;
    }
    return key.nFields() == _keyPattern.nFields();
}

void CollectionMetadata::fillRanges() {
    if (_chunksMap.empty())
        return;

    // Load the chunk information, coallesceing their ranges. The version for this shard would be
    // the highest version for any of the chunks.
    BSONObj min, max;
    for (const auto& entry : _chunksMap) {
        BSONObj currMin = entry.first;
        BSONObj currMax = entry.second.getMaxKey();

        // coalesce the chunk's bounds in ranges if they are adjacent chunks
        if (min.isEmpty()) {
            min = currMin;
            max = currMax;
            continue;
        }
        if (SimpleBSONObjComparator::kInstance.evaluate(max == currMin)) {
            max = currMax;
            continue;
        }

        _rangesMap.insert(make_pair(min, CachedChunkInfo(max, ChunkVersion::IGNORED())));

        min = currMin;
        max = currMax;
    }

    invariant(!min.isEmpty());
    invariant(!max.isEmpty());

    _rangesMap.insert(make_pair(min, CachedChunkInfo(max, ChunkVersion::IGNORED())));
}

void CollectionMetadata::fillKeyPatternFields() {
    // Parse the shard keys into the states 'keys' and 'keySet' members.
    BSONObjIterator patternIter = _keyPattern.begin();
    while (patternIter.more()) {
        BSONElement current = patternIter.next();

        _keyFields.mutableVector().push_back(new FieldRef);
        FieldRef* const newFieldRef = _keyFields.mutableVector().back();
        newFieldRef->parse(current.fieldNameStringData());
    }
}

}  // namespace mongo
