/**
 *    Copyright (C) 2010-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/s/catalog/forwarding_catalog_manager.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class BSONObj;
class CatalogCache;
class DBConfig;
class OperationContext;
class SettingsType;
class ShardRegistry;
template <typename T>
class StatusWith;


/**
 * Holds the global sharding context. Single instance exists for a running server. Exists on
 * both MongoD and MongoS.
 */
class Grid {
public:
    Grid();

    /**
     * Called at startup time so the global sharding services can be set. This method must be called
     * once and once only for the lifetime of the service.
     *
     * NOTE: Unit-tests are allowed to call it more than once, provided they reset the object's
     *       state using clearForUnitTests.
     */
    void init(std::unique_ptr<ForwardingCatalogManager> catalogManager,
              std::unique_ptr<ShardRegistry> shardRegistry,
              std::unique_ptr<ClusterCursorManager> cursorManager);

    /**
     * Implicitly creates the specified database as non-sharded.
     */
    StatusWith<std::shared_ptr<DBConfig>> implicitCreateDb(OperationContext* txn,
                                                           const std::string& dbName);

    /**
     * @return true if shards and config servers are allowed to use 'localhost' in address
     */
    bool allowLocalHost() const;

    /**
     * @param whether to allow shards and config servers to use 'localhost' in address
     */
    void setAllowLocalHost(bool allow);

    /**
     * Returns true if the balancer should be running. Caller is responsible
     * for making sure settings has the balancer key.
     */
    bool shouldBalance(const SettingsType& balancerSettings) const;

    /**
     * Returns true if the config server settings indicate that the balancer should be active.
     */
    bool getConfigShouldBalance(OperationContext* txn) const;

    /**
     * Returns a pointer to a CatalogManager to use for accessing catalog data stored on the config
     * servers.
     */
    CatalogManager* catalogManager(OperationContext* txn);

    /**
     * Returns a direct pointer to the ForwardingCatalogManager.  This should only be used for
     * calling methods that are specific to the ForwardingCatalogManager and not part of the generic
     * CatalogManager interface, such as for taking the distributed lock and scheduling replacement
     * of the underlying CatalogManager that the ForwardingCatalogManager is delegating to.
     */
    ForwardingCatalogManager* forwardingCatalogManager();

    CatalogCache* catalogCache() {
        return _catalogCache.get();
    }
    ShardRegistry* shardRegistry() {
        return _shardRegistry.get();
    }

    ClusterCursorManager* getCursorManager() {
        return _cursorManager.get();
    }

    /**
     * Clears the grid object so that it can be reused between test executions. This will not
     * be necessary if grid is hanging off the global ServiceContext and each test gets its
     * own service context.
     *
     * NOTE: Do not use this outside of unit-tests.
     */
    void clearForUnitTests();

private:
    std::unique_ptr<ForwardingCatalogManager> _catalogManager;
    std::unique_ptr<CatalogCache> _catalogCache;
    std::unique_ptr<ShardRegistry> _shardRegistry;
    std::unique_ptr<ClusterCursorManager> _cursorManager;

    // can 'localhost' be used in shard addresses?
    bool _allowLocalShard;
};

extern Grid grid;

}  // namespace mongo
