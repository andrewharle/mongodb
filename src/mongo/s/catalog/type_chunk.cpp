/**
 *    Copyright (C) 2012-2015 MongoDB Inc.
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

#include "mongo/s/catalog/type_chunk.h"

#include <cstring>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

const std::string ChunkType::ConfigNS = "config.chunks";

const BSONField<std::string> ChunkType::name("_id");
const BSONField<std::string> ChunkType::ns("ns");
const BSONField<BSONObj> ChunkType::min("min");
const BSONField<BSONObj> ChunkType::max("max");
const BSONField<std::string> ChunkType::shard("shard");
const BSONField<bool> ChunkType::jumbo("jumbo");
const BSONField<Date_t> ChunkType::DEPRECATED_lastmod("lastmod");
const BSONField<OID> ChunkType::DEPRECATED_epoch("lastmodEpoch");


StatusWith<ChunkType> ChunkType::fromBSON(const BSONObj& source) {
    ChunkType chunk;

    {
        std::string chunkName;
        Status status = bsonExtractStringField(source, name.name(), &chunkName);
        if (!status.isOK())
            return status;
        chunk._name = chunkName;
    }

    {
        std::string chunkNS;
        Status status = bsonExtractStringField(source, ns.name(), &chunkNS);
        if (!status.isOK())
            return status;
        chunk._ns = chunkNS;
    }

    {
        BSONElement chunkMinElement;
        Status status = bsonExtractTypedField(source, min.name(), Object, &chunkMinElement);
        if (!status.isOK())
            return status;
        chunk._min = chunkMinElement.Obj().getOwned();
    }

    {
        BSONElement chunkMaxElement;
        Status status = bsonExtractTypedField(source, max.name(), Object, &chunkMaxElement);
        if (!status.isOK())
            return status;
        chunk._max = chunkMaxElement.Obj().getOwned();
    }

    {
        std::string chunkShard;
        Status status = bsonExtractStringField(source, shard.name(), &chunkShard);
        if (!status.isOK())
            return status;
        chunk._shard = chunkShard;
    }

    {
        bool chunkJumbo;
        Status status = bsonExtractBooleanField(source, jumbo.name(), &chunkJumbo);
        if (status.isOK()) {
            chunk._jumbo = chunkJumbo;
        } else if (status == ErrorCodes::NoSuchKey) {
            // Jumbo status is missing, so it will be presumed false
        } else {
            return status;
        }
    }

    // The format of chunk version encoding is { lastmod: <Major|Minor>, lastmodEpoch: OID }
    if (!ChunkVersion::canParseBSON(source, DEPRECATED_lastmod())) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Unable to parse chunk version from " << source);
    }
    chunk._version = ChunkVersion::fromBSON(source, DEPRECATED_lastmod());

    return chunk;
}

Status ChunkType::validate() const {
    if (!_name.is_initialized() || _name->empty()) {
        return Status(ErrorCodes::NoSuchKey,
                      str::stream() << "missing " << name.name() << " field");
    }

    if (!_ns.is_initialized() || _ns->empty()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing " << ns.name() << " field");
    }

    if (!_min.is_initialized() || _min->isEmpty()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing " << min.name() << " field");
    }

    if (!_max.is_initialized() || _max->isEmpty()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing " << max.name() << " field");
    }

    if (!_version.is_initialized() || !_version->isSet()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing version field");
    }

    if (!_shard.is_initialized() || _shard->empty()) {
        return Status(ErrorCodes::NoSuchKey,
                      str::stream() << "missing " << shard.name() << " field");
    }

    // 'min' and 'max' must share the same fields.
    if (_min->nFields() != _max->nFields()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "min and max have a different number of keys");
    }

    BSONObjIterator minIt(getMin());
    BSONObjIterator maxIt(getMax());
    while (minIt.more() && maxIt.more()) {
        BSONElement minElem = minIt.next();
        BSONElement maxElem = maxIt.next();
        if (strcmp(minElem.fieldName(), maxElem.fieldName())) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "min and max must have the same set of keys");
        }
    }

    // 'max' should be greater than 'min'.
    if (_min->woCompare(getMax()) >= 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "max key must be greater than min key");
    }

    return Status::OK();
}

BSONObj ChunkType::toBSON() const {
    BSONObjBuilder builder;
    if (_name)
        builder.append(name.name(), getName());
    if (_ns)
        builder.append(ns.name(), getNS());
    if (_min)
        builder.append(min.name(), getMin());
    if (_max)
        builder.append(max.name(), getMax());
    if (_shard)
        builder.append(shard.name(), getShard());
    if (_version) {
        // For now, write both the deprecated *and* the new fields
        _version->addToBSON(builder, DEPRECATED_lastmod());
    }
    if (_jumbo)
        builder.append(jumbo.name(), getJumbo());

    return builder.obj();
}

std::string ChunkType::toString() const {
    return toBSON().toString();
}

void ChunkType::setName(const std::string& name) {
    invariant(!name.empty());
    _name = name;
}

void ChunkType::setNS(const std::string& ns) {
    invariant(!ns.empty());
    _ns = ns;
}

void ChunkType::setMin(const BSONObj& min) {
    invariant(!min.isEmpty());
    _min = min;
}

void ChunkType::setMax(const BSONObj& max) {
    invariant(!max.isEmpty());
    _max = max;
}

void ChunkType::setVersion(const ChunkVersion& version) {
    invariant(version.isSet());
    _version = version;
}

void ChunkType::setShard(const std::string& shard) {
    invariant(!shard.empty());
    _shard = shard;
}

void ChunkType::setJumbo(bool jumbo) {
    _jumbo = jumbo;
}

}  // namespace mongo
