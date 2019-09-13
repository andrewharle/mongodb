
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class BSONObj;
class OperationContext;
class Timestamp;

namespace repl {

class OpTime;
class StorageInterface;

/**
 * This interface provides helper functions for maintaining the documents used for
 * maintaining data consistency.
 *
 * The minValid document, in 'local.replset.minvalid', is used for indicating whether or not the
 * data on disk is consistent and for getting to a consistent point after unclean shutdown.
 *
 * Example of all fields:
 * {
 *      _id: <ObjectId>,                    // not used, but auto-generated
 *      ts: <Timestamp>,
 *      t: <long long>,                     // timestamp and term of minValid OpTime
 *      doingInitialSync: <bool>,
 *      begin: {
 *                  ts: <Timestamp>,
 *                  t: <long long>
 *             },                           // field for 'appliedThrough'
 *      oplogDeleteFromPoint: <Timestamp>,  // only exists on unclean upgrade
 *                                          // TODO (SERVER-30556): Remove after 3.6
 * }
 *
 * The oplogTruncateAfterPoint document, in 'local.replset.oplogTruncateAfterPoint', is used to
 * indicate how much of the oplog is consistent and where the oplog should be truncated when
 * entering recovery.
 *
 * Example of all fields:
 * {
 *      _id: 'oplogTruncateAfterPoint',
 *      oplogTruncateAfterPoint: <Timestamp>
 * }
 *
 * See below for explanations of each field.
 */
class ReplicationConsistencyMarkers {
    MONGO_DISALLOW_COPYING(ReplicationConsistencyMarkers);

public:
    // Constructor and Destructor.
    ReplicationConsistencyMarkers();
    virtual ~ReplicationConsistencyMarkers();

    /**
     * Initializes the minValid document with the required fields. This is safe to call on an
     * already initialized minValid document and will add any required fields that do not exist.
     */
    virtual void initializeMinValidDocument(OperationContext* opCtx) = 0;

    // -------- Initial Sync Flag ----------

    /**
     * Returns true if initial sync was started but has not completed. If we start up and this is
     * set to true, we know that we must do a resync.
     */
    virtual bool getInitialSyncFlag(OperationContext* opCtx) const = 0;

    /**
     * Sets the initial sync flag to record that initial sync has not completed.
     *
     * This operation is durable and waits for durable writes (which will block on
     * journaling/checkpointing).
     */
    virtual void setInitialSyncFlag(OperationContext* opCtx) = 0;

    /**
     * Clears the initial sync flag to record that initial sync has completed.
     *
     * This operation is durable and waits for durable writes (which will block on
     * journaling/checkpointing).
     */
    virtual void clearInitialSyncFlag(OperationContext* opCtx) = 0;

    // -------- MinValid ----------

    /**
     * The minValid value is the earliest (minimum) OpTime that must be applied in order to
     * consider the dataset consistent.
     *   - This is set to the end of a batch before we begin applying a batch of oplog entries
     *     since the oplog entries can be applied out of order.
     *   - This is also set during rollback so we do not exit RECOVERING until we are consistent.
     * If we crash while applying a batch, we apply from appliedThrough to minValid in order
     * to be consistent. We may re-apply operations, but this is safe.
     *
     * Returns the minValid OpTime.
     */
    virtual OpTime getMinValid(OperationContext* opCtx) const = 0;

    /**
     * Sets the minValid OpTime to 'minValid'. This can set minValid backwards, which is necessary
     * in rollback when the OpTimes in the oplog may move backwards.
     */
    virtual void setMinValid(OperationContext* opCtx, const OpTime& minValid) = 0;

    /**
     * Sets minValid only if it is not already higher than endOpTime.
     *
     * Warning, this compares the term and timestamp independently. Do not use if the current
     * minValid could be from the other fork of a rollback.
     */
    virtual void setMinValidToAtLeast(OperationContext* opCtx, const OpTime& minValid) = 0;

    // -------- Oplog Truncate After Point ----------

    /**
     * The oplog truncate after point is set to the beginning of a batch of oplog entries before
     * the oplog entries are written into the oplog, and reset before we begin applying the batch.
     * On startup all oplog entries with a value >= the oplog truncate after point should be
     * deleted. We write operations to the oplog in parallel so if we crash mid-batch there could
     * be holes in the oplog. Deleting them at startup keeps us consistent.
     *
     * If null, no documents should be deleted.
     *
     * If we are in feature compatibility version 3.4 and there is no oplog truncate after point
     * document, we fall back on the old oplog delete from point field in the minValid
     * collection.
     */
    virtual void setOplogTruncateAfterPoint(OperationContext* opCtx,
                                            const Timestamp& timestamp) = 0;
    virtual Timestamp getOplogTruncateAfterPoint(OperationContext* opCtx) const = 0;

    /**
     * The oplog delete from point may still exist on upgrade from an unclean shutdown. This
     * function removes the field so it's gone after 3.6.
     *
     * TODO (SERVER-30556): Delete this function in 3.8 because the old oplog delete from point
     * cannot exist.
     */
    virtual void removeOldOplogDeleteFromPointField(OperationContext* opCtx) = 0;

    // -------- Applied Through ----------

    /**
     * The applied through point is a persistent record of which oplog entries we've applied.
     * If we crash while applying a batch of oplog entries, this OpTime tells us where to start
     * applying operations on startup.
     */
    virtual void setAppliedThrough(OperationContext* opCtx, const OpTime& optime) = 0;

    /**
     * Unsets the applied through OpTime at the given 'writeTimestamp'.
     * Once cleared, the applied through point is the top of the oplog.
     */
    virtual void clearAppliedThrough(OperationContext* opCtx, const Timestamp& writeTimestamp) = 0;

    /**
     * You should probably be calling ReplicationCoordinator::getLastAppliedOpTime() instead.
     *
     * This reads the value from storage which isn't always updated when the ReplicationCoordinator
     * is. This is safe because it will only ever be stale and reapplying oplog operations is
     * always safe.
     */
    virtual OpTime getAppliedThrough(OperationContext* opCtx) const = 0;

    /**
     * Create the set of collections required for steady-state replication to work. E.g: `minvalid`
     * or `oplogTruncateAfterPoint`.
     */
    virtual Status createInternalCollections(OperationContext* opCtx) = 0;
};

}  // namespace repl
}  // namespace mongo
