
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

#include "mongo/db/jsobj.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/s/catalog/type_chunk.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

const std::string kName = "TestDB.TestColl-a_10";
const std::string kNs = "TestDB.TestColl";
const BSONObj kMin = BSON("a" << 10);
const BSONObj kMax = BSON("a" << 20);
const ShardId kFromShard("shard0000");
const ShardId kToShard("shard0001");
const bool kWaitForDelete{true};

TEST(MigrationTypeTest, ConvertFromMigrationInfo) {
    const ChunkVersion version(1, 2, OID::gen());

    BSONObjBuilder chunkBuilder;
    chunkBuilder.append(ChunkType::name(), kName);
    chunkBuilder.append(ChunkType::ns(), kNs);
    chunkBuilder.append(ChunkType::min(), kMin);
    chunkBuilder.append(ChunkType::max(), kMax);
    version.appendLegacyWithField(&chunkBuilder, ChunkType::lastmod());
    chunkBuilder.append(ChunkType::shard(), kFromShard.toString());

    ChunkType chunkType = assertGet(ChunkType::fromConfigBSON(chunkBuilder.obj()));
    ASSERT_OK(chunkType.validate());

    MigrateInfo migrateInfo(kToShard, chunkType);
    MigrationType migrationType(migrateInfo, kWaitForDelete);

    BSONObjBuilder builder;
    builder.append(MigrationType::name(), kName);
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.appendWithField(&builder, "chunkVersion");
    builder.append(MigrationType::waitForDelete(), kWaitForDelete);

    BSONObj obj = builder.obj();

    ASSERT_BSONOBJ_EQ(obj, migrationType.toBSON());
}

TEST(MigrationTypeTest, FromAndToBSON) {
    const ChunkVersion version(1, 2, OID::gen());

    BSONObjBuilder builder;
    builder.append(MigrationType::name(), kName);
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.appendWithField(&builder, "chunkVersion");
    builder.append(MigrationType::waitForDelete(), kWaitForDelete);

    BSONObj obj = builder.obj();

    MigrationType migrationType = assertGet(MigrationType::fromBSON(obj));
    ASSERT_BSONOBJ_EQ(obj, migrationType.toBSON());
}

TEST(MigrationTypeTest, MissingRequiredNamespaceField) {
    const ChunkVersion version(1, 2, OID::gen());

    BSONObjBuilder builder;
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.appendWithField(&builder, "chunkVersion");

    BSONObj obj = builder.obj();

    StatusWith<MigrationType> migrationType = MigrationType::fromBSON(obj);
    ASSERT_EQUALS(migrationType.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(migrationType.getStatus().reason(), MigrationType::ns.name());
}

TEST(MigrationTypeTest, MissingRequiredMinField) {
    const ChunkVersion version(1, 2, OID::gen());

    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.appendWithField(&builder, "chunkVersion");

    BSONObj obj = builder.obj();

    StatusWith<MigrationType> migrationType = MigrationType::fromBSON(obj);
    ASSERT_EQUALS(migrationType.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(migrationType.getStatus().reason(), MigrationType::min.name());
}

TEST(MigrationTypeTest, MissingRequiredMaxField) {
    const ChunkVersion version(1, 2, OID::gen());

    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.appendWithField(&builder, "chunkVersion");

    BSONObj obj = builder.obj();

    StatusWith<MigrationType> migrationType = MigrationType::fromBSON(obj);
    ASSERT_EQUALS(migrationType.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(migrationType.getStatus().reason(), MigrationType::max.name());
}

TEST(MigrationTypeTest, MissingRequiredFromShardField) {
    const ChunkVersion version(1, 2, OID::gen());

    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.appendWithField(&builder, "chunkVersion");

    BSONObj obj = builder.obj();

    StatusWith<MigrationType> migrationType = MigrationType::fromBSON(obj);
    ASSERT_EQUALS(migrationType.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(migrationType.getStatus().reason(), MigrationType::fromShard.name());
}

TEST(MigrationTypeTest, MissingRequiredToShardField) {
    const ChunkVersion version(1, 2, OID::gen());

    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    version.appendWithField(&builder, "chunkVersion");

    BSONObj obj = builder.obj();

    StatusWith<MigrationType> migrationType = MigrationType::fromBSON(obj);
    ASSERT_EQUALS(migrationType.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(migrationType.getStatus().reason(), MigrationType::toShard.name());
}

TEST(MigrationTypeTest, MissingRequiredVersionField) {
    BSONObjBuilder builder;
    builder.append(MigrationType::name(), kName);
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());

    BSONObj obj = builder.obj();

    StatusWith<MigrationType> migrationType = MigrationType::fromBSON(obj);
    ASSERT_EQUALS(migrationType.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(migrationType.getStatus().reason(), "chunkVersion");
}

}  // namespace
}  // namespace mongo
