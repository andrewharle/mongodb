
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
#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/time_support.h"

namespace mongo {

class LockpingsType;
class LocksType;
class OperationContext;
class Status;
template <typename T>
class StatusWith;

/**
 * Interface for the distributed lock operations.
 */
class DistLockCatalog {
    MONGO_DISALLOW_COPYING(DistLockCatalog);

public:
    static const WriteConcernOptions kLocalWriteConcern;
    static const WriteConcernOptions kMajorityWriteConcern;

    /**
     * Simple data structure for storing server local time and election id.
     */
    struct ServerInfo {
    public:
        ServerInfo(Date_t time, OID electionId);

        // The local time of the server at the time this was created.
        Date_t serverTime;

        // The election id of the replica set member at the time this was created.
        OID electionId;
    };

    virtual ~DistLockCatalog() = default;

    /**
     * Returns the ping document of the specified processID.
     * Common status errors include socket errors.
     */
    virtual StatusWith<LockpingsType> getPing(OperationContext* opCtx, StringData processID) = 0;

    /**
     * Updates the ping document. Creates a new entry if it does not exists.
     * Common status errors include socket errors.
     */
    virtual Status ping(OperationContext* opCtx, StringData processID, Date_t ping) = 0;

    /**
     * Attempts to update the owner of a lock identified by lockID to lockSessionID.
     * Will only be successful if lock is not held.
     *
     * The other parameters are for diagnostic purposes:
     * - who: unique string for the caller trying to grab the lock.
     * - processId: unique string for the process trying to grab the lock.
     * - time: the time when this is attempted.
     * - why: reason for taking the lock.
     *
     * Returns the result of the operation.
     * Returns LockStateChangeFailed if the lock acquisition cannot be done because lock
     * is already held elsewhere.
     *
     * Common status errors include socket and duplicate key errors.
     */
    virtual StatusWith<LocksType> grabLock(
        OperationContext* opCtx,
        StringData lockID,
        const OID& lockSessionID,
        StringData who,
        StringData processId,
        Date_t time,
        StringData why,
        const WriteConcernOptions& writeConcern = kMajorityWriteConcern) = 0;

    /**
     * Attempts to forcefully transfer the ownership of a lock from currentHolderTS
     * to lockSessionID.
     *
     * The other parameters are for diagnostic purposes:
     * - who: unique string for the caller trying to grab the lock.
     * - processId: unique string for the process trying to grab the lock.
     * - time: the time when this is attempted.
     * - why: reason for taking the lock.
     *
     * Returns the result of the operation.
     * Returns LockStateChangeFailed if the lock acquisition fails.
     *
     * Common status errors include socket errors.
     */
    virtual StatusWith<LocksType> overtakeLock(OperationContext* opCtx,
                                               StringData lockID,
                                               const OID& lockSessionID,
                                               const OID& currentHolderTS,
                                               StringData who,
                                               StringData processId,
                                               Date_t time,
                                               StringData why) = 0;

    /**
     * Attempts to set the state of the lock document with lockSessionID to unlocked. Returns OK,
     * if at the end of this call it is determined that the lock is definitely not owned by the
     * specified session (i.e., it is not owned at all or if it is owned by a different session).
     * Otherwise, it returns an error status. Common errors include socket errors.
     */
    virtual Status unlock(OperationContext* opCtx, const OID& lockSessionID) = 0;

    /**
     * Same as unlock() above except that it unlocks the lock document that matches "lockSessionID"
     * AND "name", rather than just "lockSessionID". This is necessary if multiple documents have
     * been locked with the same lockSessionID.
     */
    virtual Status unlock(OperationContext* opCtx, const OID& lockSessionID, StringData name) = 0;

    /**
     * Unlocks all distributed locks with the given owning process ID.  Does not provide any
     * indication as to how many locks were actually unlocked.  So long as the update command runs
     * successfully, returns OK, otherwise returns an error status.
     */
    virtual Status unlockAll(OperationContext* opCtx, const std::string& processID) = 0;

    /**
     * Get some information from the config server primary.
     * Common status errors include socket errors.
     */
    virtual StatusWith<ServerInfo> getServerInfo(OperationContext* opCtx) = 0;

    /**
     * Returns the lock document.
     * Returns LockNotFound if lock document doesn't exist.
     * Common status errors include socket errors.
     */
    virtual StatusWith<LocksType> getLockByTS(OperationContext* opCtx,
                                              const OID& lockSessionID) = 0;

    /**
     * Returns the lock document.
     * Common status errors include socket errors.
     */
    virtual StatusWith<LocksType> getLockByName(OperationContext* opCtx, StringData name) = 0;

    /**
     * Attempts to delete the ping document corresponding to the given processId.
     * Common status errors include socket errors.
     */
    virtual Status stopPing(OperationContext* opCtx, StringData processId) = 0;

protected:
    DistLockCatalog();
};

}  // namespace mongo
