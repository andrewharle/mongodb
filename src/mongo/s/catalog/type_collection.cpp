
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

#include "mongo/s/catalog/type_collection.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

const BSONField<bool> kNoBalance("noBalance");
const BSONField<bool> kDropped("dropped");
const auto kIsAssignedShardKey = "isAssignedShardKey"_sd;

}  // namespace

const NamespaceString CollectionType::ConfigNS("config.collections");

const BSONField<std::string> CollectionType::fullNs("_id");
const BSONField<OID> CollectionType::epoch("lastmodEpoch");
const BSONField<Date_t> CollectionType::updatedAt("lastmod");
const BSONField<BSONObj> CollectionType::keyPattern("key");
const BSONField<BSONObj> CollectionType::defaultCollation("defaultCollation");
const BSONField<bool> CollectionType::unique("unique");
const BSONField<UUID> CollectionType::uuid("uuid");

StatusWith<CollectionType> CollectionType::fromBSON(const BSONObj& source) {
    CollectionType coll;

    {
        std::string collFullNs;
        Status status = bsonExtractStringField(source, fullNs.name(), &collFullNs);
        if (!status.isOK())
            return status;

        coll._fullNs = NamespaceString{collFullNs};
    }

    {
        OID collEpoch;
        Status status = bsonExtractOIDFieldWithDefault(source, epoch.name(), OID(), &collEpoch);
        if (!status.isOK())
            return status;

        coll._epoch = collEpoch;
    }

    {
        BSONElement collUpdatedAt;
        Status status = bsonExtractTypedField(source, updatedAt.name(), Date, &collUpdatedAt);
        if (!status.isOK())
            return status;

        coll._updatedAt = collUpdatedAt.Date();
    }

    {
        bool collDropped;
        Status status = bsonExtractBooleanField(source, kDropped.name(), &collDropped);
        if (status.isOK()) {
            coll._dropped = collDropped;
        } else if (status == ErrorCodes::NoSuchKey) {
            // Dropped can be missing in which case it is presumed false
        } else {
            return status;
        }
    }

    {
        BSONElement collKeyPattern;
        Status status = bsonExtractTypedField(source, keyPattern.name(), Object, &collKeyPattern);
        if (status.isOK()) {
            BSONObj obj = collKeyPattern.Obj();
            if (obj.isEmpty()) {
                return Status(ErrorCodes::ShardKeyNotFound, "empty shard key");
            }

            coll._keyPattern = KeyPattern(obj.getOwned());
        } else if (status == ErrorCodes::NoSuchKey) {
            // Sharding key can only be missing if the collection is dropped
            if (!coll.getDropped()) {
                return {ErrorCodes::NoSuchKey,
                        str::stream() << "Shard key for collection " << coll._fullNs->ns()
                                      << " is missing, but the collection is not marked as "
                                         "dropped. This is an indication of corrupted sharding "
                                         "metadata."};
            }
        } else {
            return status;
        }
    }

    {
        BSONElement collDefaultCollation;
        Status status =
            bsonExtractTypedField(source, defaultCollation.name(), Object, &collDefaultCollation);
        if (status.isOK()) {
            BSONObj obj = collDefaultCollation.Obj();
            if (obj.isEmpty()) {
                return Status(ErrorCodes::BadValue, "empty defaultCollation");
            }

            coll._defaultCollation = obj.getOwned();
        } else if (status != ErrorCodes::NoSuchKey) {
            return status;
        }
    }

    {
        bool collUnique;
        Status status = bsonExtractBooleanField(source, unique.name(), &collUnique);
        if (status.isOK()) {
            coll._unique = collUnique;
        } else if (status == ErrorCodes::NoSuchKey) {
            // Key uniqueness can be missing in which case it is presumed false
        } else {
            return status;
        }
    }

    {
        BSONElement uuidElem;
        Status status = bsonExtractField(source, uuid.name(), &uuidElem);
        if (status.isOK()) {
            auto swUUID = UUID::parse(uuidElem);
            if (!swUUID.isOK()) {
                return swUUID.getStatus();
            }
            coll._uuid = swUUID.getValue();
        } else if (status == ErrorCodes::NoSuchKey) {
            // UUID can be missing in 3.6, because featureCompatibilityVersion can be 3.4, in which
            // case it remains boost::none.
        } else {
            return status;
        }
    }

    {
        bool collNoBalance;
        Status status = bsonExtractBooleanField(source, kNoBalance.name(), &collNoBalance);
        if (status.isOK()) {
            coll._allowBalance = !collNoBalance;
        } else if (status == ErrorCodes::NoSuchKey) {
            // No balance can be missing in which case it is presumed as false
        } else {
            return status;
        }
    }

    {
        bool isAssignedShardKey;
        Status status = bsonExtractBooleanField(source, kIsAssignedShardKey, &isAssignedShardKey);
        if (status.isOK()) {
            coll._isAssignedShardKey = isAssignedShardKey;
        } else if (status == ErrorCodes::NoSuchKey) {
            // isAssignedShardKey can be missing in which case it is presumed as true.
        } else {
            return status;
        }
    }

    return StatusWith<CollectionType>(coll);
}

Status CollectionType::validate() const {
    // These fields must always be set
    if (!_fullNs.is_initialized()) {
        return Status(ErrorCodes::NoSuchKey, "missing ns");
    }

    if (!_fullNs->isValid()) {
        return Status(ErrorCodes::BadValue, "invalid namespace " + _fullNs->toString());
    }

    if (!_epoch.is_initialized()) {
        return Status(ErrorCodes::NoSuchKey, "missing epoch");
    }

    if (!_updatedAt.is_initialized()) {
        return Status(ErrorCodes::NoSuchKey, "missing updated at timestamp");
    }

    if (!_dropped.get_value_or(false)) {
        if (!_epoch->isSet()) {
            return Status(ErrorCodes::BadValue, "invalid epoch");
        }

        if (Date_t() == _updatedAt.get()) {
            return Status(ErrorCodes::BadValue, "invalid updated at timestamp");
        }

        if (!_keyPattern.is_initialized()) {
            return Status(ErrorCodes::NoSuchKey, "missing key pattern");
        } else {
            invariant(!_keyPattern->toBSON().isEmpty());
        }
    }

    return Status::OK();
}

BSONObj CollectionType::toBSON() const {
    BSONObjBuilder builder;

    if (_fullNs) {
        builder.append(fullNs.name(), _fullNs->toString());
    }
    builder.append(epoch.name(), _epoch.get_value_or(OID()));
    builder.append(updatedAt.name(), _updatedAt.get_value_or(Date_t()));
    builder.append(kDropped.name(), _dropped.get_value_or(false));

    // These fields are optional, so do not include them in the metadata for the purposes of
    // consuming less space on the config servers.

    if (_keyPattern.is_initialized()) {
        builder.append(keyPattern.name(), _keyPattern->toBSON());
    }

    if (!_defaultCollation.isEmpty()) {
        builder.append(defaultCollation.name(), _defaultCollation);
    }

    if (_unique.is_initialized()) {
        builder.append(unique.name(), _unique.get());
    }

    if (_uuid.is_initialized()) {
        _uuid->appendToBuilder(&builder, uuid.name());
    }

    if (_allowBalance.is_initialized()) {
        builder.append(kNoBalance.name(), !_allowBalance.get());
    }

    if (_isAssignedShardKey) {
        builder.append(kIsAssignedShardKey, !_isAssignedShardKey.get());
    }

    return builder.obj();
}

std::string CollectionType::toString() const {
    return toBSON().toString();
}

void CollectionType::setNs(const NamespaceString& fullNs) {
    invariant(fullNs.isValid());
    _fullNs = fullNs;
}

void CollectionType::setEpoch(OID epoch) {
    _epoch = epoch;
}

void CollectionType::setUpdatedAt(Date_t updatedAt) {
    _updatedAt = updatedAt;
}

void CollectionType::setKeyPattern(const KeyPattern& keyPattern) {
    invariant(!keyPattern.toBSON().isEmpty());
    _keyPattern = keyPattern;
}

bool CollectionType::hasSameOptions(CollectionType& other) {
    // The relevant options must have been set on this CollectionType.
    invariant(_fullNs && _keyPattern && _unique);

    return *_fullNs == other.getNs() &&
        SimpleBSONObjComparator::kInstance.evaluate(_keyPattern->toBSON() ==
                                                    other.getKeyPattern().toBSON()) &&
        SimpleBSONObjComparator::kInstance.evaluate(_defaultCollation ==
                                                    other.getDefaultCollation()) &&
        *_unique == other.getUnique();
}

}  // namespace mongo
