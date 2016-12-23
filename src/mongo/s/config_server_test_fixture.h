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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/s/sharding_mongod_test_fixture.h"

namespace mongo {

class BSONObj;
class ChunkType;
class NamespaceString;
class Shard;
class ShardingCatalogClient;
class ShardingCatalogManager;
class ShardRegistry;
class ShardType;
template <typename T>
class StatusWith;

/**
 * Provides config-specific functionality in addition to the mock storage engine and mock network
 * provided by ShardingMongodTestFixture.
 */
class ConfigServerTestFixture : public ShardingMongodTestFixture {
public:
    ConfigServerTestFixture();
    ~ConfigServerTestFixture();

    std::shared_ptr<Shard> getConfigShard() const;

    /**
     * Insert a document to this config server to the specified namespace.
     */
    Status insertToConfigCollection(OperationContext* txn,
                                    const NamespaceString& ns,
                                    const BSONObj& doc);

    /**
     * Reads a single document from a collection living on the config server.
     */
    StatusWith<BSONObj> findOneOnConfigCollection(OperationContext* txn,
                                                  const NamespaceString& ns,
                                                  const BSONObj& filter);

    /**
     * Setup the config.shards collection to contain the given shards.
     */
    Status setupShards(const std::vector<ShardType>& shards);

    /**
     * Retrieves the shard document from the config server.
     * Returns {ErrorCodes::ShardNotFound} if the given shard does not exists.
     */
    StatusWith<ShardType> getShardDoc(OperationContext* txn, const std::string& shardId);

    /**
     * Setup the config.chunks collection to contain the given chunks.
     */
    Status setupChunks(const std::vector<ChunkType>& chunks);

    /**
     * Retrieves the chunk document from the config server.
     */
    StatusWith<ChunkType> getChunkDoc(OperationContext* txn, const BSONObj& minKey);

    /**
     * Returns the indexes definitions defined on a given collection.
     */
    StatusWith<std::vector<BSONObj>> getIndexes(OperationContext* txn, const NamespaceString& ns);

    /**
     * Returns the stored raw pointer to the addShard TaskExecutor's NetworkInterface.
     */
    executor::NetworkInterfaceMock* networkForAddShard() const;

    /**
     * Returns the stored raw pointer to the addShard TaskExecutor.
     */
    executor::TaskExecutor* executorForAddShard() const;

    /**
     * Same as ShardingMongodTestFixture::onCommand but run against _addShardNetworkTestEnv.
     */
    void onCommandForAddShard(executor::NetworkTestEnv::OnCommandFunction func);

protected:
    /**
     * Sets this node up as a mongod with sharding components for ClusterRole::ConfigServer.
     */
    void setUp() override;

    std::unique_ptr<DistLockCatalog> makeDistLockCatalog(ShardRegistry* shardRegistry) override;

    std::unique_ptr<DistLockManager> makeDistLockManager(
        std::unique_ptr<DistLockCatalog> distLockCatalog) override;

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override;

    std::unique_ptr<ShardingCatalogManager> makeShardingCatalogManager(
        ShardingCatalogClient* catalogClient) override;

    std::unique_ptr<CatalogCache> makeCatalogCache() override;

    std::unique_ptr<ClusterCursorManager> makeClusterCursorManager() override;

    std::unique_ptr<BalancerConfiguration> makeBalancerConfiguration() override;

private:
    // Since these are currently private members of the real ShardingCatalogManager, we store a raw
    // pointer to them here.
    executor::NetworkInterfaceMock* _mockNetworkForAddShard;
    executor::TaskExecutor* _executorForAddShard;

    // Allows for processing tasks through the NetworkInterfaceMock/ThreadPoolMock subsystem.
    std::unique_ptr<executor::NetworkTestEnv> _addShardNetworkTestEnv;
};

}  // namespace mongo
