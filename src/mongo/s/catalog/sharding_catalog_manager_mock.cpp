/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/sharding_catalog_manager_mock.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"

namespace mongo {

using std::string;
using std::vector;

ShardingCatalogManagerMock::ShardingCatalogManagerMock() = default;

ShardingCatalogManagerMock::~ShardingCatalogManagerMock() = default;

Status ShardingCatalogManagerMock::startup() {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

void ShardingCatalogManagerMock::shutDown(OperationContext* txn) {}

StatusWith<string> ShardingCatalogManagerMock::addShard(
    OperationContext* txn,
    const std::string* shardProposedName,
    const ConnectionString& shardConnectionString,
    const long long maxSize) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogManagerMock::addShardToZone(OperationContext* txn,
                                                  const std::string& shardName,
                                                  const std::string& zoneName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogManagerMock::removeShardFromZone(OperationContext* txn,
                                                       const std::string& shardName,
                                                       const std::string& zoneName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogManagerMock::assignKeyRangeToZone(OperationContext* txn,
                                                        const NamespaceString& ns,
                                                        const ChunkRange& range,
                                                        const std::string& zoneName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogManagerMock::removeKeyRangeFromZone(OperationContext* txn,
                                                          const NamespaceString& ns,
                                                          const ChunkRange& range) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogManagerMock::commitChunkSplit(OperationContext* txn,
                                                    const NamespaceString& ns,
                                                    const OID& requestEpoch,
                                                    const ChunkRange& range,
                                                    const std::vector<BSONObj>& splitPoints,
                                                    const std::string& shardName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogManagerMock::commitChunkMerge(OperationContext* txn,
                                                    const NamespaceString& ns,
                                                    const OID& requestEpoch,
                                                    const std::vector<BSONObj>& chunkBoundaries,
                                                    const std::string& shardName) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

void ShardingCatalogManagerMock::appendConnectionStats(executor::ConnectionPoolStats* stats) {}

Status ShardingCatalogManagerMock::initializeConfigDatabaseIfNeeded(OperationContext* txn) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

void ShardingCatalogManagerMock::discardCachedConfigDatabaseInitializationState() {}

Status ShardingCatalogManagerMock::initializeShardingAwarenessOnUnawareShards(
    OperationContext* txn) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status ShardingCatalogManagerMock::upsertShardIdentityOnShard(OperationContext* txn,
                                                              ShardType shardType) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

BSONObj ShardingCatalogManagerMock::createShardIdentityUpsertForAddShard(
    OperationContext* txn, const std::string& shardName) {
    MONGO_UNREACHABLE;
}

void ShardingCatalogManagerMock::cancelAddShardTaskIfNeeded(const ShardId& shardId) {
    MONGO_UNREACHABLE;
}

Status ShardingCatalogManagerMock::setFeatureCompatibilityVersionOnShards(
    OperationContext* txn, const std::string& version) {
    MONGO_UNREACHABLE;
}

}  // namespace mongo
