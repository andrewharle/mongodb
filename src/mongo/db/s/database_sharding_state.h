
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
#include "mongo/db/catalog/database.h"
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/s/database_version_gen.h"

namespace mongo {

class MovePrimarySourceManager;
class OperationContext;

/**
 * Synchronizes access to this shard server's cached database version for Database.
 */
class DatabaseShardingState {
    MONGO_DISALLOW_COPYING(DatabaseShardingState);

public:
    static const Database::Decoration<DatabaseShardingState> get;

    DatabaseShardingState();
    ~DatabaseShardingState() = default;

    /**
     * Methods to control the databases's critical section. Must be called with the database X lock
     * held.
     */
    void enterCriticalSectionCatchUpPhase(OperationContext* opCtx);
    void enterCriticalSectionCommitPhase(OperationContext* opCtx);
    void exitCriticalSection(OperationContext* opCtx,
                             boost::optional<DatabaseVersion> newDbVersion);

    auto getCriticalSectionSignal(ShardingMigrationCriticalSection::Operation op) const {
        return _critSec.getSignal(op);
    }

    /**
     * Returns this shard server's cached dbVersion, if one is cached.
     *
     * Invariants that the caller holds the DBLock in X or IS.
     */
    boost::optional<DatabaseVersion> getDbVersion(OperationContext* opCtx) const;

    /**
     * Sets this shard server's cached dbVersion to newVersion.
     *
     * Invariants that the caller holds the DBLock in X mode.
     */
    void setDbVersion(OperationContext* opCtx, boost::optional<DatabaseVersion> newVersion);

    /**
     * If _critSecSignal is non-null, always throws StaleDbVersion.
     * Otherwise, if there is a client dbVersion on the OperationContext, compares it with this
     * shard server's cached dbVersion and throws StaleDbVersion if they do not match.
     */
    void checkDbVersion(OperationContext* opCtx) const;

    /**
     * Returns the active movePrimary source manager, if one is available.
     */
    MovePrimarySourceManager* getMovePrimarySourceManager();

    /**
     * Attaches a movePrimary source manager to this database's sharding state. Must be called with
     * the database lock in X mode. May not be called if there is a movePrimary source manager
     * already installed. Must be followed by a call to clearMovePrimarySourceManager.
     */
    void setMovePrimarySourceManager(OperationContext* opCtx, MovePrimarySourceManager* sourceMgr);

    /**
     * Removes a movePrimary source manager from this database's sharding state. Must be called with
     * with the database lock in X mode. May not be called if there isn't a movePrimary source
     * manager installed already through a previous call to setMovePrimarySourceManager.
     */
    void clearMovePrimarySourceManager(OperationContext* opCtx);

private:
    // Modifying the state below requires holding the DBLock in X mode; holding the DBLock in any
    // mode is acceptable for reading it. (Note: accessing this class at all requires holding the
    // DBLock in some mode, since it requires having a pointer to the Database).

    ShardingMigrationCriticalSection _critSec;

    // This shard server's cached dbVersion. If boost::none, indicates this shard server does not
    // know the dbVersion.
    boost::optional<DatabaseVersion> _dbVersion = boost::none;

    // If this database is serving as a source shard for a movePrimary, the source manager will be
    // non-null. To write this value, there needs to be X-lock on the database in order to
    // synchronize with other callers which will read the source manager.
    //
    // NOTE: The source manager is not owned by this class.
    MovePrimarySourceManager* _sourceMgr{nullptr};
};

}  // namespace mongo
