/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include <string>

#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {

/**
 * Creates and parses commit chunk migration command BSON objects.
 */
class CommitChunkMigrationRequest {
public:
    /**
     * Parses the input command and produces a request corresponding to its arguments.
     */
    static StatusWith<CommitChunkMigrationRequest> createFromCommand(const NamespaceString& nss,
                                                                     const BSONObj& obj);

    /**
     * Constructs a commitChunkMigration command with the specified parameters and writes it to
     * the builder, without closing the builder. The builder must be empty, but callers are free
     * to append more fields once the command has been constructed.
     */
    static void appendAsCommand(BSONObjBuilder* builder,
                                const NamespaceString& nss,
                                const ShardId& fromShard,
                                const ShardId& toShard,
                                const ChunkType& migratedChunkType,
                                const boost::optional<ChunkType>& controlChunkType,
                                const ChunkVersion& fromShardChunkVersion,
                                const bool& shardHasDistributedLock);

    const NamespaceString& getNss() const {
        return _nss;
    }

    const ShardId& getFromShard() const {
        return _fromShard;
    }

    const ShardId& getToShard() const {
        return _toShard;
    }

    const ChunkRange& getMigratedChunkRange() const {
        return _migratedChunkRange;
    }

    const ChunkRange& getControlChunkRange() const;

    bool hasControlChunkRange() {
        return bool(_controlChunkRange);
    }

    const ChunkVersion& getFromShardCollectionVersion() const {
        return _fromShardCollectionVersion;
    }

    bool shardHasDistributedLock() {
        return _shardHasDistributedLock;
    }

private:
    CommitChunkMigrationRequest(const NamespaceString& nss, const ChunkRange& range);

    // The collection for which this request applies.
    NamespaceString _nss;

    // The source shard name.
    ShardId _fromShard;

    // The recipient shard name.
    ShardId _toShard;

    // Range of migrated chunk being moved.
    ChunkRange _migratedChunkRange;

    // Range of control chunk being moved, if it exists.
    boost::optional<ChunkRange> _controlChunkRange;

    // Collection version of the source shard.
    ChunkVersion _fromShardCollectionVersion;

    // Flag to indicate whether the shard has the distlock.
    bool _shardHasDistributedLock;
};

}  // namespace mongo
