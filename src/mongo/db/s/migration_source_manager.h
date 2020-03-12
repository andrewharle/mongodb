
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

#pragma once

#include <boost/optional.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/s/request_types/move_chunk_request.h"
#include "mongo/util/timer.h"

namespace mongo {

class OperationContext;
struct ShardingStatistics;

/**
 * The donor-side migration state machine. This object must be created and owned by a single thread,
 * which controls its lifetime and should not be passed across threads. Unless explicitly indicated
 * its methods must not be called from more than one thread and must not be called while any locks
 * are held.
 *
 * The intended workflow is as follows:
 *  - Acquire a distributed lock on the collection whose chunk is about to be moved.
 *  - Instantiate a MigrationSourceManager on the stack. This will snapshot the latest collection
 *      metadata, which should stay stable because of the distributed collection lock.
 *  - Call startClone to initiate background cloning of the chunk contents. This will perform the
 *      necessary registration of the cloner with the replication subsystem and will start listening
 *      for document changes, while at the same time responding to data fetch requests from the
 *      recipient.
 *  - Call awaitUntilCriticalSectionIsAppropriate to wait for the cloning process to catch up
 *      sufficiently so we don't keep the server in read-only state for too long.
 *  - Call enterCriticalSection to cause the shard to enter in 'read only' mode while the latest
 *      changes are drained by the recipient shard.
 *  - Call commitDonateChunk to commit the chunk move in the config server's metadata and leave the
 *      read only (critical section) mode.
 *
 * At any point in time it is safe to let the MigrationSourceManager object go out of scope in which
 * case the desctructor will take care of clean up based on how far we have advanced. One exception
 * is the commitDonateChunk and its comments explain the reasoning.
 */
class MigrationSourceManager {
    MONGO_DISALLOW_COPYING(MigrationSourceManager);

public:
    static MigrationSourceManager* get(CollectionShardingRuntime& csr);
    static MigrationSourceManager* get(CollectionShardingRuntime* csr) {
        return get(*csr);
    }

    /**
     * Instantiates a new migration source manager with the specified migration parameters. Must be
     * called with the distributed lock acquired in advance (not asserted).
     *
     * Loads the most up-to-date collection metadata and uses it as a starting point. It is assumed
     * that because of the distributed lock, the collection's metadata will not change further.
     *
     * May throw any exception. Known exceptions are:
     *  - InvalidOptions if the operation context is missing shard version
     *  - StaleConfigException if the expected collection version does not match what we find it
     *      to be after acquiring the distributed lock.
     */
    MigrationSourceManager(OperationContext* opCtx,
                           MoveChunkRequest request,
                           ConnectionString donorConnStr,
                           HostAndPort recipientHost);
    ~MigrationSourceManager();

    /**
     * Returns the namespace for which this source manager is active.
     */
    NamespaceString getNss() const;

    /**
     * Contacts the donor shard and tells it to start cloning the specified chunk. This method will
     * fail if for any reason the donor shard fails to initiate the cloning sequence.
     *
     * Expected state: kCreated
     * Resulting state: kCloning on success, kDone on failure
     */
    Status startClone(OperationContext* opCtx);

    /**
     * Waits for the cloning to catch up sufficiently so we won't have to stay in the critical
     * section for a long period of time. This method will fail if any error occurs while the
     * recipient is catching up.
     *
     * Expected state: kCloning
     * Resulting state: kCloneCaughtUp on success, kDone on failure
     */
    Status awaitToCatchUp(OperationContext* opCtx);

    /**
     * Waits for the active clone operation to catch up and enters critical section. Once this call
     * returns successfully, no writes will be happening on this shard until the chunk donation is
     * committed. Therefore, commitChunkOnRecipient/commitChunkMetadata must be called as soon as
     * possible afterwards.
     *
     * Expected state: kCloneCaughtUp
     * Resulting state: kCriticalSection on success, kDone on failure
     */
    Status enterCriticalSection(OperationContext* opCtx);

    /**
     * Tells the recipient of the chunk to commit the chunk contents, which it received.
     *
     * Expected state: kCriticalSection
     * Resulting state: kCloneCompleted on success, kDone on failure
     */
    Status commitChunkOnRecipient(OperationContext* opCtx);

    /**
     * Tells the recipient shard to fetch the latest portion of data from the donor and to commit it
     * locally. After that it persists the changed metadata to the config servers and leaves the
     * critical section.
     *
     * NOTE: Since we cannot recover from failures to write chunk metadata to the config servers, if
     *       applying the committed chunk information fails and we cannot definitely verify that the
     *       write was definitely applied or not, this call may crash the server.
     *
     * Expected state: kCloneCompleted
     * Resulting state: kDone
     */
    Status commitChunkMetadataOnConfig(OperationContext* opCtx);

    /**
     * May be called at any time. Unregisters the migration source manager from the collection,
     * restores the committed metadata (if in critical section) and logs error in the change log to
     * indicate that the migration has failed.
     *
     * Expected state: Any
     * Resulting state: kDone
     */
    void cleanupOnError(OperationContext* opCtx);

    /**
     * Returns the cloner which is being used for this migration. This value is available only if
     * the migration source manager is currently in the clone phase (i.e. the previous call to
     * startClone has succeeded).
     *
     * Must be called with some form of lock on the collection namespace.
     */
    MigrationChunkClonerSource* getCloner() const {
        return _cloneDriver.get();
    }

    /**
     * Returns a report on the active migration.
     *
     * Must be called with some form of lock on the collection namespace.
     */
    BSONObj getMigrationStatusReport() const;

private:
    // Used to track the current state of the source manager. See the methods above, which have
    // comments explaining the various state transitions.
    enum State { kCreated, kCloning, kCloneCaughtUp, kCriticalSection, kCloneCompleted, kDone };

    ScopedCollectionMetadata _getCurrentMetadataAndCheckEpoch(OperationContext* opCtx);

    /**
     * If this donation moves the first chunk to the recipient (i.e., the recipient didn't have any
     * chunks), this function writes a no-op message to the oplog, so that change stream will notice
     * that and close the cursor in order to notify mongos to target the new shard as well.
     */
    void _notifyChangeStreamsOnRecipientFirstChunk(OperationContext* opCtx,
                                                   const ScopedCollectionMetadata& metadata);

    /**
     * Called when any of the states fails. May only be called once and will put the migration
     * manager into the kDone state.
     */
    void _cleanup(OperationContext* opCtx);

    // The parameters to the moveChunk command
    const MoveChunkRequest _args;

    // The resolved connection string of the donor shard
    const ConnectionString _donorConnStr;

    // The resolved primary of the recipient shard
    const HostAndPort _recipientHost;

    // Stores a reference to the process sharding statistics object which needs to be updated
    ShardingStatistics& _stats;

    // Times the entire moveChunk operation
    const Timer _entireOpTimer;

    // Starts counting from creation time and is used to time various parts from the lifetime of the
    // move chunk sequence
    Timer _cloneAndCommitTimer;

    // The current state. Used only for diagnostics and validation.
    State _state{kCreated};

    // The version of the collection at the time migration started.
    OID _collectionEpoch;

    // The UUID of the the collection whose chunks are being moved. Default to empty if the
    // collection doesn't have UUID.
    boost::optional<UUID> _collectionUuid;

    // The chunk cloner source. Only available if there is an active migration going on. To set and
    // remove it, global S lock needs to be acquired first in order to block all logOp calls and
    // then the mutex. To access it, only the mutex is necessary. Available after cloning stage has
    // completed.
    std::unique_ptr<MigrationChunkClonerSource> _cloneDriver;

    // The statistics about a chunk migration to be included in moveChunk.commit
    BSONObj _recipientCloneCounts;

    boost::optional<CollectionCriticalSection> _critSec;
};

}  // namespace mongo
