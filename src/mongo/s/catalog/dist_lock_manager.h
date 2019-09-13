
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
#include "mongo/stdx/chrono.h"

namespace mongo {

using DistLockHandle = OID;
class OperationContext;
class Status;
template <typename T>
class StatusWith;

/**
 * Interface for handling distributed locks.
 *
 * Usage:
 *
 * auto scopedDistLock = mgr->lock(...);
 *
 * if (!scopedDistLock.isOK()) {
 *   // Did not get lock. scopedLockStatus destructor will not call unlock.
 * }
 *
 * // To check if lock is still owned:
 * auto status = scopedDistLock.getValue().checkStatus();
 *
 * if (!status.isOK()) {
 *   // Someone took over the lock! Unlock will still be called at destructor, but will
 *   // practically be a no-op since it doesn't own the lock anymore.
 * }
 */
class DistLockManager {
public:
    // Default timeout which will be used if one is not passed to the lock method.
    static const Seconds kDefaultLockTimeout;

    // Timeout value, which specifies that if the lock is not available immediately, no attempt
    // should be made to wait for it to become free.
    static const Milliseconds kSingleLockAttemptTimeout;

    /**
     * RAII type for distributed lock. Not meant to be shared across multiple threads.
     */
    class ScopedDistLock {
        MONGO_DISALLOW_COPYING(ScopedDistLock);

    public:
        ScopedDistLock(OperationContext* opCtx,
                       DistLockHandle lockHandle,
                       DistLockManager* lockManager);
        ~ScopedDistLock();

        ScopedDistLock(ScopedDistLock&& other);
        ScopedDistLock& operator=(ScopedDistLock&& other);

        /**
         * Checks whether the lock is still being held by querying the config server.
         */
        Status checkStatus();

    private:
        OperationContext* _opCtx;
        DistLockHandle _lockID;
        DistLockManager* _lockManager;  // Not owned here.
    };

    virtual ~DistLockManager() = default;

    /**
     * Performs bootstrapping for the manager. Implementation do not need to guarantee
     * thread safety so callers should employ proper synchronization when calling this method.
     */
    virtual void startUp() = 0;

    /**
     * Cleanup the manager's resources. Implementations do not need to guarantee thread safety
     * so callers should employ proper synchronization when calling this method.
     */
    virtual void shutDown(OperationContext* opCtx) = 0;

    /**
     * Returns the process ID for this DistLockManager.
     */
    virtual std::string getProcessID() = 0;

    /**
     * Tries multiple times to lock, using the specified lock try interval, until
     * a certain amount of time has passed or when any error that is not LockBusy
     * occurred.
     *
     * waitFor = 0 indicates there should only be one attempt to acquire the lock, and
     * no waiting.
     * waitFor = -1 indicates we should retry indefinitely.
     *
     * Returns OK if the lock was successfully acquired.
     * Returns ErrorCodes::DistributedClockSkewed when a clock skew is detected.
     * Returns ErrorCodes::LockBusy if the lock is being held.
     */
    StatusWith<ScopedDistLock> lock(OperationContext* opCtx,
                                    StringData name,
                                    StringData whyMessage,
                                    Milliseconds waitFor);

    /**
     * Same behavior as lock(...) above, except takes a specific lock session ID "lockSessionID"
     * instead of randomly generating one internally.
     *
     * This is useful for a process running on the config primary after a failover. A lock can be
     * immediately reacquired if "lockSessionID" matches that of the lock, rather than waiting for
     * the inactive lock to expire.
     */
    virtual StatusWith<DistLockHandle> lockWithSessionID(OperationContext* opCtx,
                                                         StringData name,
                                                         StringData whyMessage,
                                                         const OID& lockSessionID,
                                                         Milliseconds waitFor) = 0;

    /**
     * Specialized locking method, which only succeeds if the specified lock name is not held by
     * anyone. Uses local write concern and does not attempt to overtake the lock or check whether
     * the lock lease has expired.
     */
    virtual StatusWith<DistLockHandle> tryLockWithLocalWriteConcern(OperationContext* opCtx,
                                                                    StringData name,
                                                                    StringData whyMessage,
                                                                    const OID& lockSessionID) = 0;

    /**
     * Unlocks the given lockHandle. Will attempt to retry again later if the config
     * server is not reachable.
     */
    virtual void unlock(OperationContext* opCtx, const DistLockHandle& lockHandle) = 0;

    /**
     * Unlocks the lock specified by "lockHandle" and "name". Will attempt to retry again later if
     * the config server is not reachable.
     */
    virtual void unlock(OperationContext* opCtx,
                        const DistLockHandle& lockHandle,
                        StringData name) = 0;

    /**
     * Makes a best-effort attempt to unlock all locks owned by the given processID.
     */
    virtual void unlockAll(OperationContext* opCtx, const std::string& processID) = 0;

protected:
    /**
     * Checks if the lockHandle still exists in the config server.
     */
    virtual Status checkStatus(OperationContext* opCtx, const DistLockHandle& lockHandle) = 0;
};

}  // namespace mongo
