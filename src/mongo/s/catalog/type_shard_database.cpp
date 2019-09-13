
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

#include "mongo/s/catalog/type_shard_database.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/util/assert_util.h"

namespace mongo {

const BSONField<std::string> ShardDatabaseType::name("_id");
const BSONField<DatabaseVersion> ShardDatabaseType::version("version");
const BSONField<std::string> ShardDatabaseType::primary("primary");
const BSONField<bool> ShardDatabaseType::partitioned("partitioned");
const BSONField<int> ShardDatabaseType::enterCriticalSectionCounter("enterCriticalSectionCounter");

ShardDatabaseType::ShardDatabaseType(const std::string dbName,
                                     boost::optional<DatabaseVersion> version,
                                     const ShardId primary,
                                     bool partitioned)
    : _name(dbName), _version(version), _primary(primary), _partitioned(partitioned) {}

StatusWith<ShardDatabaseType> ShardDatabaseType::fromBSON(const BSONObj& source) {
    std::string dbName;
    {
        Status status = bsonExtractStringField(source, name.name(), &dbName);
        if (!status.isOK())
            return status;
    }

    boost::optional<DatabaseVersion> dbVersion = boost::none;
    {
        BSONObj versionField = source.getObjectField("version");
        // TODO: Parse this unconditionally once featureCompatibilityVersion 3.6 is no longer
        // supported.
        if (!versionField.isEmpty()) {
            dbVersion = DatabaseVersion::parse(IDLParserErrorContext("DatabaseType"), versionField);
        }
    }

    std::string dbPrimary;
    {
        Status status = bsonExtractStringField(source, primary.name(), &dbPrimary);
        if (!status.isOK())
            return status;
    }

    bool dbPartitioned;
    {
        Status status =
            bsonExtractBooleanFieldWithDefault(source, partitioned.name(), false, &dbPartitioned);
        if (!status.isOK())
            return status;
    }

    ShardDatabaseType shardDatabaseType(dbName, dbVersion, dbPrimary, dbPartitioned);

    return shardDatabaseType;
}

BSONObj ShardDatabaseType::toBSON() const {
    BSONObjBuilder builder;

    builder.append(name.name(), _name);
    if (_version) {
        builder.append(version.name(), _version->toBSON());
    }
    builder.append(primary.name(), _primary.toString());
    builder.append(partitioned.name(), _partitioned);

    return builder.obj();
}

std::string ShardDatabaseType::toString() const {
    return toBSON().toString();
}

void ShardDatabaseType::setDbVersion(boost::optional<DatabaseVersion> version) {
    _version = version;
}

void ShardDatabaseType::setDbName(const std::string& dbName) {
    invariant(!dbName.empty());
    _name = dbName;
}

void ShardDatabaseType::setPrimary(const ShardId& primary) {
    invariant(primary.isValid());
    _primary = primary;
}

void ShardDatabaseType::setPartitioned(bool partitioned) {
    _partitioned = partitioned;
}

}  // namespace mongo
