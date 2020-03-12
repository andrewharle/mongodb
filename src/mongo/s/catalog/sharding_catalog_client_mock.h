
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

#include "mongo/s/catalog/sharding_catalog_client.h"

namespace mongo {

/**
 * A dummy implementation of ShardingCatalogClient for testing purposes.
 */
class ShardingCatalogClientMock : public ShardingCatalogClient {
public:
    ShardingCatalogClientMock(std::unique_ptr<DistLockManager> distLockManager);
    ~ShardingCatalogClientMock();

    void startup() override;

    void shutDown(OperationContext* opCtx) override;

    StatusWith<repl::OpTimeWith<DatabaseType>> getDatabase(
        OperationContext* opCtx,
        const std::string& dbName,
        repl::ReadConcernLevel readConcernLevel) override;

    StatusWith<repl::OpTimeWith<std::vector<DatabaseType>>> getAllDBs(
        OperationContext* opCtx, repl::ReadConcernLevel readConcern) override;

    StatusWith<repl::OpTimeWith<CollectionType>> getCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        repl::ReadConcernLevel readConcernLevel) override;

    StatusWith<std::vector<CollectionType>> getCollections(
        OperationContext* opCtx,
        const std::string* dbName,
        repl::OpTime* optime,
        repl::ReadConcernLevel readConcernLevel) override;

    std::vector<NamespaceString> getAllShardedCollectionsForDb(
        OperationContext* opCtx, StringData dbName, repl::ReadConcernLevel readConcern) override;

    StatusWith<std::vector<std::string>> getDatabasesForShard(OperationContext* opCtx,
                                                              const ShardId& shardName) override;

    StatusWith<std::vector<ChunkType>> getChunks(OperationContext* opCtx,
                                                 const BSONObj& filter,
                                                 const BSONObj& sort,
                                                 boost::optional<int> limit,
                                                 repl::OpTime* opTime,
                                                 repl::ReadConcernLevel readConcern) override;

    StatusWith<std::vector<TagsType>> getTagsForCollection(OperationContext* opCtx,
                                                           const NamespaceString& nss) override;

    StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
        OperationContext* opCtx, repl::ReadConcernLevel readConcern) override;

    bool runUserManagementWriteCommand(OperationContext* opCtx,
                                       const std::string& commandName,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       BSONObjBuilder* result) override;

    bool runUserManagementReadCommand(OperationContext* opCtx,
                                      const std::string& dbname,
                                      const BSONObj& cmdObj,
                                      BSONObjBuilder* result) override;

    Status applyChunkOpsDeprecated(OperationContext* opCtx,
                                   const BSONArray& updateOps,
                                   const BSONArray& preCondition,
                                   const NamespaceString& nss,
                                   const ChunkVersion& lastChunkVersion,
                                   const WriteConcernOptions& writeConcern,
                                   repl::ReadConcernLevel readConcern) override;

    Status logAction(OperationContext* opCtx,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail) override;

    Status logChange(OperationContext* opCtx,
                     const std::string& what,
                     const std::string& ns,
                     const BSONObj& detail,
                     const WriteConcernOptions& writeConcern) override;

    StatusWith<BSONObj> getGlobalSettings(OperationContext* opCtx, StringData key) override;

    StatusWith<VersionType> getConfigVersion(OperationContext* opCtx,
                                             repl::ReadConcernLevel readConcern) override;

    void writeConfigServerDirect(OperationContext* opCtx,
                                 const BatchedCommandRequest& request,
                                 BatchedCommandResponse* response) override;

    Status insertConfigDocument(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const BSONObj& doc,
                                const WriteConcernOptions& writeConcern) override;

    void insertConfigDocumentsAsRetryableWrite(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               std::vector<BSONObj> docs,
                                               const WriteConcernOptions& writeConcern) override;

    StatusWith<bool> updateConfigDocument(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& query,
                                          const BSONObj& update,
                                          bool upsert,
                                          const WriteConcernOptions& writeConcern) override;

    Status removeConfigDocuments(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const BSONObj& query,
                                 const WriteConcernOptions& writeConcern) override;

    Status createDatabase(OperationContext* opCtx, StringData dbName, ShardId primaryShard);

    DistLockManager* getDistLockManager() override;

    StatusWith<std::vector<KeysCollectionDocument>> getNewKeys(
        OperationContext* opCtx,
        StringData purpose,
        const LogicalTime& newerThanThis,
        repl::ReadConcernLevel readConcernLevel) override;

private:
    std::unique_ptr<DistLockManager> _distLockManager;

    StatusWith<repl::OpTimeWith<std::vector<BSONObj>>> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcern,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit) override;
};

}  // namespace mongo
