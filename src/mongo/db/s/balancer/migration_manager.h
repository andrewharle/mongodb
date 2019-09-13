
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

#include <list>
#include <map>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class OperationContext;
class ScopedMigrationRequest;
class ServiceContext;
class Status;
template <typename T>
class StatusWith;

// Uniquely identifies a migration, regardless of shard and version.
typedef std::string MigrationIdentifier;
typedef std::map<MigrationIdentifier, Status> MigrationStatuses;

/**
 * Manages and executes parallel migrations for the balancer.
 */
class MigrationManager {
    MONGO_DISALLOW_COPYING(MigrationManager);

public:
    MigrationManager(ServiceContext* serviceContext);
    ~MigrationManager();

    /**
     * A blocking method that attempts to schedule all the migrations specified in
     * "candidateMigrations" and wait for them to complete. Takes the distributed lock for each
     * collection with a chunk being migrated.
     *
     * If any of the migrations, which were scheduled in parallel fails with a LockBusy error
     * reported from the shard, retries it serially without the distributed lock.
     *
     * Returns a map of migration Status objects to indicate the success/failure of each migration.
     */
    MigrationStatuses executeMigrationsForAutoBalance(
        OperationContext* opCtx,
        const std::vector<MigrateInfo>& migrateInfos,
        uint64_t maxChunkSizeBytes,
        const MigrationSecondaryThrottleOptions& secondaryThrottle,
        bool waitForDelete);

    /**
     * A blocking method that attempts to schedule the migration specified in "migrateInfo" and
     * waits for it to complete. Takes the distributed lock for the namespace which is being
     * migrated.
     *
     * Returns the status of the migration.
     */
    Status executeManualMigration(OperationContext* opCtx,
                                  const MigrateInfo& migrateInfo,
                                  uint64_t maxChunkSizeBytes,
                                  const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                  bool waitForDelete);

    /**
     * Non-blocking method that puts the migration manager in the kRecovering state, in which
     * new migration requests will block until finishRecovery is called. Then reacquires distributed
     * locks for the balancer and any active migrations. The distributed locks are taken with local
     * write concern, since this is called in drain mode where majority writes are not yet possible.
     *
     * The active migration recovery may fail and be abandoned, setting the state to kEnabled.
     */
    void startRecoveryAndAcquireDistLocks(OperationContext* opCtx);

    /**
     * Blocking method that must only be called after startRecovery has been called. Recovers the
     * state of the migration manager (if necessary and able) and puts it in the kEnabled state,
     * where it will accept new migrations. Any migrations waiting on the recovery state will be
     * unblocked once the state is kEnabled, and then this function waits for the recovered active
     * migrations to finish before returning.
     *
     * The active migration recovery may fail and be abandoned, setting the state to kEnabled and
     * unblocking any process waiting on the recovery state.
     */
    void finishRecovery(OperationContext* opCtx,
                        uint64_t maxChunkSizeBytes,
                        const MigrationSecondaryThrottleOptions& secondaryThrottle);

    /**
     * Non-blocking method that should never be called concurrently with finishRecovery. Puts the
     * manager in a state where all subsequently scheduled migrations will immediately fail (without
     * ever getting scheduled) and all active ones will be cancelled. It has no effect if the
     * migration manager is already stopping or stopped.
     */
    void interruptAndDisableMigrations();

    /**
     * Blocking method that waits for any currently scheduled migrations to complete. Must be
     * called after interruptAndDisableMigrations has been called in order to be able to re-enable
     * migrations again.
     */
    void drainActiveMigrations();

private:
    // The current state of the migration manager
    enum class State {  // Allowed transitions:
        kStopped,       // kRecovering
        kRecovering,    // kEnabled, kStopping
        kEnabled,       // kStopping
        kStopping,      // kStopped
    };

    /**
     * Tracks the execution state of a single migration.
     */
    struct Migration {
        Migration(NamespaceString nss, BSONObj moveChunkCmdObj);
        ~Migration();

        // Namespace for which this migration applies
        NamespaceString nss;

        // Command object representing the migration
        BSONObj moveChunkCmdObj;

        // Callback handle for the migration network request. If the migration has not yet been sent
        // on the network, this value is not set.
        boost::optional<executor::TaskExecutor::CallbackHandle> callbackHandle;

        // Notification, which will be signaled when the migration completes
        std::shared_ptr<Notification<executor::RemoteCommandResponse>> completionNotification;
    };

    // Used as a type in which to store a list of active migrations. The reason to choose list is
    // that its iterators do not get invalidated when entries are removed around them. This allows
    // O(1) removal time.
    using MigrationsList = std::list<Migration>;

    using CollectionMigrationsStateMap = stdx::unordered_map<NamespaceString, MigrationsList>;

    /**
     * Optionally takes the collection distributed lock and schedules a chunk migration with the
     * specified parameters. May block for distributed lock acquisition. If dist lock acquisition is
     * successful (or not done), schedules the migration request and returns a notification which
     * can be used to obtain the outcome of the operation.
     */
    std::shared_ptr<Notification<executor::RemoteCommandResponse>> _schedule(
        OperationContext* opCtx,
        const MigrateInfo& migrateInfo,
        uint64_t maxChunkSizeBytes,
        const MigrationSecondaryThrottleOptions& secondaryThrottle,
        bool waitForDelete);

    /**
     * Acquires the collection distributed lock for the specified namespace and if it succeeds,
     * schedules the migration.
     *
     * The distributed lock is acquired before scheduling the first migration for the collection and
     * is only released when all active migrations on the collection have finished.
     */
    void _schedule(WithLock,
                   OperationContext* opCtx,
                   const HostAndPort& targetHost,
                   Migration migration);

    /**
     * Used internally for migrations scheduled with the distributed lock acquired by the config
     * server. Called exactly once for each scheduled migration, it will signal the migration in the
     * passed iterator and if this is the last migration for the collection will free the collection
     * distributed lock.
     */
    void _complete(WithLock,
                   OperationContext* opCtx,
                   MigrationsList::iterator itMigration,
                   const executor::RemoteCommandResponse& remoteCommandResponse);

    /**
     * If the state of the migration manager is kStopping, checks whether there are any outstanding
     * scheduled requests and if there aren't any signals the class condition variable.
     */
    void _checkDrained(WithLock);

    /**
     * Blocking call, which waits for the migration manager to leave the recovering state (if it is
     * currently recovering).
     */
    void _waitForRecovery();

    /**
     * Should only be called from startRecovery or finishRecovery functions when the migration
     * manager is in either the kStopped or kRecovering state. Releases all the distributed locks
     * that the balancer holds, clears the config.migrations collection, changes the state of the
     * migration manager to kEnabled. Then unblocks all processes waiting for kEnabled state.
     */
    void _abandonActiveMigrationsAndEnableManager(OperationContext* opCtx);

    /**
     * Parses a moveChunk RemoteCommandResponse's two levels of Status objects and distiguishes
     * between errors generated by this config server and the shard primary to which the moveChunk
     * command was sent.
     *
     * If the command failed because of stepdown of this config server, the migration document
     * managed by 'scopedMigrationRequest' is saved for later balancer recovery and a
     * BalancerInterrupted error is returned. If the command failed because the shard to which the
     * command was sent returned an error, the migration document is not saved and the error is
     * returned without conversion.
     */
    Status _processRemoteCommandResponse(
        const executor::RemoteCommandResponse& remoteCommandResponse,
        ScopedMigrationRequest* scopedMigrationRequest);

    // The service context under which this migration manager runs.
    ServiceContext* const _serviceContext;

    // Used as a constant session ID for all distributed locks that this MigrationManager holds.
    // Currently required so that locks can be reacquired for the balancer in startRecovery and then
    // overtaken in later operations.
    const OID _lockSessionID{OID::gen()};

    // Carries migration information over from startRecovery to finishRecovery. Should only be set
    // in startRecovery and then accessed in finishRecovery.
    stdx::unordered_map<NamespaceString, std::list<MigrationType>> _migrationRecoveryMap;

    // Protects the class state below.
    stdx::mutex _mutex;

    // Always start the migration manager in a stopped state.
    State _state{State::kStopped};

    // Condition variable, which is waited on when the migration manager's state is changing and
    // signaled when the state change is complete.
    stdx::condition_variable _condVar;

    // Maps collection namespaces to that collection's active migrations.
    CollectionMigrationsStateMap _activeMigrations;
};

}  // namespace mongo
