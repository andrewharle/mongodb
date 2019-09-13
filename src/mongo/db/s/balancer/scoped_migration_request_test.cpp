
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/scoped_migration_request.h"

#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"

namespace mongo {
namespace {

using unittest::assertGet;

const std::string kNs = "TestDB.TestColl";
const BSONObj kMin = BSON("a" << 10);
const BSONObj kMax = BSON("a" << 20);
const ShardId kFromShard("shard0000");
const ShardId kToShard("shard0001");
const ShardId kDifferentToShard("shard0002");
const std::string kName = "TestDB.TestColl-a_10";

class ScopedMigrationRequestTest : public ConfigServerTestFixture {
public:
    /**
     * Queries config.migrations for a document with name (_id) "chunkName" and asserts that the
     * number of documents returned equals "expectedNumberOfDocuments".
     */
    void checkMigrationsCollectionForDocument(std::string chunkName,
                                              const unsigned long expectedNumberOfDocuments);

    /**
     * Makes a ScopedMigrationRequest and checks that the migration was written to
     * config.migrations. This exercises the ScopedMigrationRequest move and assignment
     * constructors.
     */
    ScopedMigrationRequest makeScopedMigrationRequest(const MigrateInfo& migrateInfo);
};

void ScopedMigrationRequestTest::checkMigrationsCollectionForDocument(
    std::string chunkName, const unsigned long expectedNumberOfDocuments) {
    auto response = shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        operationContext(),
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kMajorityReadConcern,
        MigrationType::ConfigNS,
        BSON(MigrationType::name(chunkName)),
        BSONObj(),
        boost::none);
    Shard::QueryResponse queryResponse = unittest::assertGet(response);
    std::vector<BSONObj> docs = queryResponse.docs;
    ASSERT_EQUALS(expectedNumberOfDocuments, docs.size());
}

ScopedMigrationRequest ScopedMigrationRequestTest::makeScopedMigrationRequest(
    const MigrateInfo& migrateInfo) {
    ScopedMigrationRequest scopedMigrationRequest =
        assertGet(ScopedMigrationRequest::writeMigration(operationContext(), migrateInfo, false));

    checkMigrationsCollectionForDocument(migrateInfo.getName(), 1);

    return scopedMigrationRequest;
}

MigrateInfo makeMigrateInfo() {
    const ChunkVersion kChunkVersion{1, 2, OID::gen()};

    BSONObjBuilder chunkBuilder;
    chunkBuilder.append(ChunkType::name(), kName);
    chunkBuilder.append(ChunkType::ns(), kNs);
    chunkBuilder.append(ChunkType::min(), kMin);
    chunkBuilder.append(ChunkType::max(), kMax);
    kChunkVersion.appendLegacyWithField(&chunkBuilder, ChunkType::lastmod());
    chunkBuilder.append(ChunkType::shard(), kFromShard.toString());

    ChunkType chunkType = assertGet(ChunkType::fromConfigBSON(chunkBuilder.obj()));
    ASSERT_OK(chunkType.validate());

    return MigrateInfo(kToShard, chunkType);
}

TEST_F(ScopedMigrationRequestTest, CreateScopedMigrationRequest) {
    MigrateInfo migrateInfo = makeMigrateInfo();

    {
        ScopedMigrationRequest scopedMigrationRequest = assertGet(
            ScopedMigrationRequest::writeMigration(operationContext(), migrateInfo, false));

        checkMigrationsCollectionForDocument(migrateInfo.getName(), 1);
    }

    checkMigrationsCollectionForDocument(migrateInfo.getName(), 0);
}

/**
 * A document is created via scoped object, but document is not removed in destructor because
 * keepDocumentOnDestruct() is called beforehand. Then recreate the scoped object without writing to
 * the migraitons collection, and remove on destruct.
 *
 * Simulates (mostly) Balancer recovery.
 */
TEST_F(ScopedMigrationRequestTest, CreateScopedMigrationRequestOnRecovery) {
    MigrateInfo migrateInfo = makeMigrateInfo();

    // Insert the document for the MigrationRequest and then prevent its removal in the destructor.
    {
        ScopedMigrationRequest scopedMigrationRequest = assertGet(
            ScopedMigrationRequest::writeMigration(operationContext(), migrateInfo, false));

        checkMigrationsCollectionForDocument(migrateInfo.getName(), 1);

        scopedMigrationRequest.keepDocumentOnDestruct();
    }

    checkMigrationsCollectionForDocument(migrateInfo.getName(), 1);

    // Fail to write a migration document if a migration document already exists for that chunk but
    // with a different destination shard. (the migration request must have identical parameters).
    {
        MigrateInfo differentToShardMigrateInfo = migrateInfo;
        differentToShardMigrateInfo.to = kDifferentToShard;

        StatusWith<ScopedMigrationRequest> statusWithScopedMigrationRequest =
            ScopedMigrationRequest::writeMigration(
                operationContext(), differentToShardMigrateInfo, false);

        ASSERT_EQUALS(ErrorCodes::DuplicateKey, statusWithScopedMigrationRequest.getStatus());

        checkMigrationsCollectionForDocument(migrateInfo.getName(), 1);
    }

    // Create a new scoped object without inserting a document, and check that the destructor
    // still removes the document corresponding to the MigrationRequest.
    {
        ScopedMigrationRequest scopedMigrationRequest = ScopedMigrationRequest::createForRecovery(
            operationContext(), migrateInfo.nss, migrateInfo.minKey);

        checkMigrationsCollectionForDocument(migrateInfo.getName(), 1);
    }

    checkMigrationsCollectionForDocument(migrateInfo.getName(), 0);
}

TEST_F(ScopedMigrationRequestTest, CreateMultipleScopedMigrationRequestsForIdenticalMigration) {
    MigrateInfo migrateInfo = makeMigrateInfo();

    {
        // Create a ScopedMigrationRequest, which will do the config.migrations write.
        ScopedMigrationRequest scopedMigrationRequest = assertGet(
            ScopedMigrationRequest::writeMigration(operationContext(), migrateInfo, false));

        checkMigrationsCollectionForDocument(migrateInfo.getName(), 1);

        {
            // Should be able to create another Scoped object if the request is identical.
            ScopedMigrationRequest identicalScopedMigrationRequest = assertGet(
                ScopedMigrationRequest::writeMigration(operationContext(), migrateInfo, false));

            checkMigrationsCollectionForDocument(migrateInfo.getName(), 1);
        }

        // If any scoped object goes out of scope, the migration should be over and the document
        // removed.
        checkMigrationsCollectionForDocument(migrateInfo.getName(), 0);
    }

    checkMigrationsCollectionForDocument(migrateInfo.getName(), 0);
}

TEST_F(ScopedMigrationRequestTest, TryToRemoveScopedMigrationRequestBeforeDestruct) {
    MigrateInfo migrateInfo = makeMigrateInfo();

    // Remove the migration document with tryToRemoveMigration().
    ScopedMigrationRequest scopedMigrationRequest =
        assertGet(ScopedMigrationRequest::writeMigration(operationContext(), migrateInfo, false));

    checkMigrationsCollectionForDocument(migrateInfo.getName(), 1);

    ASSERT_OK(scopedMigrationRequest.tryToRemoveMigration());

    checkMigrationsCollectionForDocument(migrateInfo.getName(), 0);
}

TEST_F(ScopedMigrationRequestTest, MoveAndAssignmentConstructors) {
    MigrateInfo migrateInfo = makeMigrateInfo();

    // Test that when the move and assignment constructors are used and the original variable goes
    // out of scope, the original object's destructor does not remove the migration document.
    {
        ScopedMigrationRequest anotherScopedMigrationRequest =
            makeScopedMigrationRequest(migrateInfo);

        checkMigrationsCollectionForDocument(migrateInfo.getName(), 1);
    }

    checkMigrationsCollectionForDocument(migrateInfo.getName(), 0);
}

}  // namespace
}  // namespace mongo
