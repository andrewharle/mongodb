
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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"

namespace mongo {

class BSONObj;
class OperationContext;

/**
 * Startup parameter to ignore featureCompatibilityVersion checks. This parameter cannot be set if
 * the node is started with --replSet. This should never be set by end users.
 */
extern bool internalValidateFeaturesAsMaster;

class FeatureCompatibilityVersion {
public:
    /**
     * Should be taken in shared mode by any operations that should not run while
     * setFeatureCompatibilityVersion is running.
     *
     * setFCV takes this lock in exclusive mode so that it both does not run with the shared mode
     * operations and does not run with itself.
     */
    static Lock::ResourceMutex fcvLock;

    /**
     * Records intent to perform a 3.6 -> 4.0 upgrade by updating the on-disk feature
     * compatibility version document to have 'version'=3.6, 'targetVersion'=4.0.
     * Should be called before schemas are modified.
     */
    static void setTargetUpgrade(OperationContext* opCtx);

    /**
     * Records intent to perform a 4.0 -> 3.6 downgrade by updating the on-disk feature
     * compatibility version document to have 'version'=3.6, 'targetVersion'=3.6.
     * Should be called before schemas are modified.
     */
    static void setTargetDowngrade(OperationContext* opCtx);

    /**
     * Records the completion of a 3.6 <-> 4.0 upgrade or downgrade by updating the on-disk feature
     * compatibility version document to have 'version'=version and unsetting the 'targetVersion'
     * field.
     * Should be called after schemas are modified.
     */
    static void unsetTargetUpgradeOrDowngrade(OperationContext* opCtx, StringData version);

    /**
     * If there are no non-local databases, store the featureCompatibilityVersion document. If we
     * are not running with --shardsvr, set the version to be the upgrade value. If we are running
     * with --shardsvr, set the version to be the downgrade value.
     */
    static void setIfCleanStartup(OperationContext* opCtx,
                                  repl::StorageInterface* storageInterface);

    /**
     * Returns true if the server is on a clean startup. A clean startup means there are no
     * databases on disk besides the local database.
     */
    static bool isCleanStartUp();

    /**
     * Examines a document inserted or updated in the server configuration collection
     * (admin.system.version). If it is the featureCompatibilityVersion document, validates the
     * document and on commit, updates the server parameter.
     */
    static void onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc);

    /**
     * Sets the server's outgoing and incomingInternalClient minWireVersions according to the
     * current featureCompatibilityVersion value.
     */
    static void updateMinWireVersion();

private:
    /**
     * Validate version. Uasserts if invalid.
     */
    static void _validateVersion(StringData version);

    /**
     * Build update command.
     */
    typedef stdx::function<void(BSONObjBuilder)> UpdateBuilder;
    static void _runUpdateCommand(OperationContext* opCtx, UpdateBuilder callback);
};


}  // namespace mongo
