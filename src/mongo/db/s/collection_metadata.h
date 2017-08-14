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

#pragma once

#include "mongo/db/range_arithmetic.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {

class ChunkType;

/**
 * The collection metadata has metadata information about a collection, in particular the
 * sharding information. It's main goal in life is to be capable of answering if a certain
 * document belongs to it or not. (In some scenarios such as chunk migration, a given
 * document is in a shard but cannot be accessed.)
 *
 * To build a collection from config data, please check the MetadataLoader. The methods
 * here allow building a new incarnation of a collection's metadata based on an existing
 * one (e.g, we're splitting in a given collection.).
 *
 * This class is immutable once constructed.
 */
class CollectionMetadata {
public:
    /**
     * The main way to construct CollectionMetadata is through MetadataLoader or the clone*()
     * methods.
     *
     * The constructors should not be used directly outside of tests.
     */
    CollectionMetadata(const BSONObj& keyPattern,
                       ChunkVersion collectionVersion,
                       ChunkVersion shardVersion,
                       RangeMap shardChunksMap);

    ~CollectionMetadata();

    /**
     * Returns a new metadata's instance based on 'this's state by removing a 'pending' chunk.
     *
     * The shard and collection version of the new metadata are unaffected.  The caller owns the
     * new metadata.
     */
    std::unique_ptr<CollectionMetadata> cloneMinusPending(const ChunkType& chunk) const;

    /**
     * Returns a new metadata's instance based on 'this's state by adding a 'pending' chunk.
     *
     * The shard and collection version of the new metadata are unaffected.  The caller owns the
     * new metadata.
     */
    std::unique_ptr<CollectionMetadata> clonePlusPending(const ChunkType& chunk) const;

    /**
     * Returns true if the document key 'key' is a valid instance of a shard key for this
     * metadata.  The 'key' must contain exactly the same fields as the shard key pattern.
     */
    bool isValidKey(const BSONObj& key) const;

    /**
     * Returns true if the document key 'key' belongs to this chunkset. Recall that documents of
     * an in-flight chunk migration may be present and should not be considered part of the
     * collection / chunkset yet. Key must be the full shard key.
     */
    bool keyBelongsToMe(const BSONObj& key) const;

    /**
     * Returns true if the document key 'key' is or has been migrated to this shard, and may
     * belong to us after a subsequent config reload.  Key must be the full shard key.
     */
    bool keyIsPending(const BSONObj& key) const;

    /**
     * Given a key 'lookupKey' in the shard key range, get the next chunk which overlaps or is
     * greater than this key.  Returns true if a chunk exists, false otherwise.
     *
     * Passing a key that is not a valid shard key for this range results in undefined behavior.
     */
    bool getNextChunk(const BSONObj& lookupKey, ChunkType* chunk) const;

    /**
     * Given a chunk identifying key "chunkMinKey", finds a different chunk if one exists.
     */
    bool getDifferentChunk(const BSONObj& chunkMinKey, ChunkType* differentChunk) const;

    /**
     * Validates that the passed-in chunk's bounds exactly match a chunk in the metadata cache.
     */
    Status checkChunkIsValid(const ChunkType& chunk);

    /**
     * Given a key in the shard key range, get the next range which overlaps or is greater than
     * this key.
     *
     * This allows us to do the following to iterate over all orphan ranges:
     *
     * KeyRange range;
     * BSONObj lookupKey = metadata->getMinKey();
     * while( metadata->getNextOrphanRange( lookupKey, &orphanRange ) ) {
     *   // Do stuff with range
     *   lookupKey = orphanRange.maxKey;
     * }
     *
     * @param lookupKey passing a key that does not belong to this metadata is undefined.
     * @param orphanRange the output range. Note that the NS is not set.
     */
    bool getNextOrphanRange(const BSONObj& lookupKey, KeyRange* orphanRange) const;

    ChunkVersion getCollVersion() const {
        return _collVersion;
    }

    ChunkVersion getShardVersion() const {
        return _shardVersion;
    }

    const RangeMap& getChunks() const {
        return _chunksMap;
    }

    const BSONObj& getKeyPattern() const {
        return _shardKeyPattern.toBSON();
    }

    const std::vector<FieldRef*>& getKeyPatternFields() const {
        return _shardKeyPattern.getKeyPatternFields();
    }

    BSONObj getMinKey() const;

    BSONObj getMaxKey() const;

    std::size_t getNumChunks() const {
        return _chunksMap.size();
    }

    /**
     * BSON output of the basic metadata information (chunk and shard version).
     */
    void toBSONBasic(BSONObjBuilder& bb) const;

    /**
     * BSON output of the chunks metadata into a BSONArray
     */
    void toBSONChunks(BSONArrayBuilder& bb) const;

    /**
     * BSON output of the pending metadata into a BSONArray
     */
    void toBSONPending(BSONArrayBuilder& bb) const;

    /**
     * String output of the collection and shard versions.
     */
    std::string toStringBasic() const;

private:
    // Shard key pattern for the collection
    ShardKeyPattern _shardKeyPattern;

    // a version for this collection that identifies the collection incarnation (ie, a
    // dropped and recreated collection with the same name would have a different version)
    ChunkVersion _collVersion;

    // highest ChunkVersion for which this metadata's information is accurate
    ChunkVersion _shardVersion;

    // Map of chunks tracked by this shard
    RangeMap _chunksMap;

    // Map of ranges of chunks that are migrating but have not been confirmed added yet
    RangeMap _pendingMap;

    // A second map from a min key into a range or contiguous chunks. The map is redundant
    // w.r.t. _chunkMap but we expect high chunk contiguity, especially in small
    // installations.
    RangeMap _rangesMap;
};

}  // namespace mongo
