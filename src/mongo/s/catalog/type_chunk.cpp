
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

#include "mongo/s/catalog/type_chunk.h"

#include <cstring>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

const NamespaceString ChunkType::ConfigNS("config.chunks");
const std::string ChunkType::ShardNSPrefix = "config.cache.chunks.";

const BSONField<std::string> ChunkType::name("_id");
const BSONField<BSONObj> ChunkType::minShardID("_id");
const BSONField<std::string> ChunkType::ns("ns");
const BSONField<BSONObj> ChunkType::min("min");
const BSONField<BSONObj> ChunkType::max("max");
const BSONField<std::string> ChunkType::shard("shard");
const BSONField<bool> ChunkType::jumbo("jumbo");
const BSONField<Date_t> ChunkType::lastmod("lastmod");
const BSONField<OID> ChunkType::epoch("lastmodEpoch");
const BSONField<BSONObj> ChunkType::history("history");

namespace {

const char kMinKey[] = "min";
const char kMaxKey[] = "max";

/**
 * Extracts an Object value from 'obj's field 'fieldName'. Sets the result to 'bsonElement'.
 */
Status extractObject(const BSONObj& obj, const std::string& fieldName, BSONElement* bsonElement) {
    Status elementStatus = bsonExtractTypedField(obj, fieldName, Object, bsonElement);
    if (!elementStatus.isOK()) {
        return elementStatus.withContext(str::stream() << "The field '" << fieldName
                                                       << "' cannot be parsed");
    }

    if (bsonElement->Obj().isEmpty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "The field '" << fieldName << "' cannot be empty"};
    }

    return Status::OK();
}

}  // namespace

ChunkRange::ChunkRange(BSONObj minKey, BSONObj maxKey)
    : _minKey(std::move(minKey)), _maxKey(std::move(maxKey)) {
    dassert(SimpleBSONObjComparator::kInstance.evaluate(_minKey < _maxKey),
            str::stream() << "Illegal chunk range: " << _minKey.toString() << ", "
                          << _maxKey.toString());
}

StatusWith<ChunkRange> ChunkRange::fromBSON(const BSONObj& obj) {
    BSONElement minKey;
    {
        Status minKeyStatus = extractObject(obj, kMinKey, &minKey);
        if (!minKeyStatus.isOK()) {
            return minKeyStatus;
        }
    }

    BSONElement maxKey;
    {
        Status maxKeyStatus = extractObject(obj, kMaxKey, &maxKey);
        if (!maxKeyStatus.isOK()) {
            return maxKeyStatus;
        }
    }

    if (SimpleBSONObjComparator::kInstance.evaluate(minKey.Obj() >= maxKey.Obj())) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "min: " << minKey.Obj() << " should be less than max: "
                              << maxKey.Obj()};
    }

    return ChunkRange(minKey.Obj().getOwned(), maxKey.Obj().getOwned());
}

bool ChunkRange::containsKey(const BSONObj& key) const {
    return _minKey.woCompare(key) <= 0 && key.woCompare(_maxKey) < 0;
}

void ChunkRange::append(BSONObjBuilder* builder) const {
    builder->append(kMinKey, _minKey);
    builder->append(kMaxKey, _maxKey);
}

const Status ChunkRange::extractKeyPattern(KeyPattern* shardKeyPatternOut) const {
    BSONObjIterator min(getMin());
    BSONObjIterator max(getMax());
    BSONObjBuilder b;
    while (min.more() && max.more()) {
        BSONElement x = min.next();
        BSONElement y = max.next();
        if (!str::equals(x.fieldName(), y.fieldName()) || (min.more() && !max.more()) ||
            (!min.more() && max.more())) {
            return {ErrorCodes::ShardKeyNotFound,
                    str::stream() << "the shard key of min " << _minKey << " doesn't match with "
                                  << "the shard key of max "
                                  << _maxKey};
        }
        b.append(x.fieldName(), 1);
    }
    const auto& shardKeyPattern = KeyPattern(b.obj());
    *shardKeyPatternOut = shardKeyPattern;
    return Status::OK();
}

std::string ChunkRange::toString() const {
    return str::stream() << "[" << _minKey << ", " << _maxKey << ")";
}

bool ChunkRange::operator==(const ChunkRange& other) const {
    return _minKey.woCompare(other._minKey) == 0 && _maxKey.woCompare(other._maxKey) == 0;
}

bool ChunkRange::operator!=(const ChunkRange& other) const {
    return !(*this == other);
}

bool ChunkRange::covers(ChunkRange const& other) const {
    auto le = [](auto const& a, auto const& b) { return a.woCompare(b) <= 0; };
    return le(_minKey, other._minKey) && le(other._maxKey, _maxKey);
}

boost::optional<ChunkRange> ChunkRange::overlapWith(ChunkRange const& other) const {
    auto le = [](auto const& a, auto const& b) { return a.woCompare(b) <= 0; };
    if (le(other._maxKey, _minKey) || le(_maxKey, other._minKey)) {
        return boost::none;
    }
    return ChunkRange(le(_minKey, other._minKey) ? other._minKey : _minKey,
                      le(_maxKey, other._maxKey) ? _maxKey : other._maxKey);
}

ChunkRange ChunkRange::unionWith(ChunkRange const& other) const {
    auto le = [](auto const& a, auto const& b) { return a.woCompare(b) <= 0; };
    return ChunkRange(le(_minKey, other._minKey) ? _minKey : other._minKey,
                      le(_maxKey, other._maxKey) ? other._maxKey : _maxKey);
}

StatusWith<std::vector<ChunkHistory>> ChunkHistory::fromBSON(const BSONArray& source) {
    std::vector<ChunkHistory> values;

    for (const auto& arrayElement : source) {
        if (arrayElement.type() == Object) {
            IDLParserErrorContext tempContext("chunk history array");
            values.emplace_back(ChunkHistoryBase::parse(tempContext, arrayElement.Obj()));
        } else {
            return {ErrorCodes::BadValue,
                    str::stream() << "array element does not have the object type: "
                                  << arrayElement.type()};
        }
    }

    return values;
}

// ChunkType

ChunkType::ChunkType() = default;

ChunkType::ChunkType(NamespaceString nss, ChunkRange range, ChunkVersion version, ShardId shardId)
    : _nss(nss),
      _min(range.getMin()),
      _max(range.getMax()),
      _version(version),
      _shard(std::move(shardId)) {}

StatusWith<ChunkType> ChunkType::fromConfigBSON(const BSONObj& source) {
    ChunkType chunk;

    {
        std::string chunkNS;
        Status status = bsonExtractStringField(source, ns.name(), &chunkNS);
        if (!status.isOK())
            return status;
        chunk._nss = NamespaceString(chunkNS);
    }

    {
        auto chunkRangeStatus = ChunkRange::fromBSON(source);
        if (!chunkRangeStatus.isOK())
            return chunkRangeStatus.getStatus();

        const auto chunkRange = std::move(chunkRangeStatus.getValue());
        chunk._min = chunkRange.getMin().getOwned();
        chunk._max = chunkRange.getMax().getOwned();
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

    {
        auto versionStatus = ChunkVersion::parseLegacyWithField(source, ChunkType::lastmod());
        if (!versionStatus.isOK()) {
            return versionStatus.getStatus();
        }
        chunk._version = std::move(versionStatus.getValue());
    }

    {
        BSONElement historyObj;
        Status status = bsonExtractTypedField(source, history.name(), Array, &historyObj);
        if (status.isOK()) {
            auto history = ChunkHistory::fromBSON(BSONArray(historyObj.Obj()));
            if (!history.isOK())
                return history.getStatus();

            chunk._history = std::move(history.getValue());
        } else if (status == ErrorCodes::NoSuchKey) {
            // History is missing, so it will be presumed empty
        } else {
            return status;
        }
    }

    return chunk;
}

BSONObj ChunkType::toConfigBSON() const {
    BSONObjBuilder builder;
    if (_nss && _min)
        builder.append(name.name(), getName());
    if (_nss)
        builder.append(ns.name(), getNS().ns());
    if (_min)
        builder.append(min.name(), getMin());
    if (_max)
        builder.append(max.name(), getMax());
    if (_shard)
        builder.append(shard.name(), getShard().toString());
    if (_version)
        _version->appendLegacyWithField(&builder, ChunkType::lastmod());
    if (_jumbo)
        builder.append(jumbo.name(), getJumbo());
    addHistoryToBSON(builder);
    return builder.obj();
}

StatusWith<ChunkType> ChunkType::fromShardBSON(const BSONObj& source, const OID& epoch) {
    ChunkType chunk;

    {
        BSONElement minKey;
        Status minKeyStatus = extractObject(source, minShardID.name(), &minKey);
        if (!minKeyStatus.isOK()) {
            return minKeyStatus;
        }

        BSONElement maxKey;
        Status maxKeyStatus = extractObject(source, max.name(), &maxKey);
        if (!maxKeyStatus.isOK()) {
            return maxKeyStatus;
        }

        if (SimpleBSONObjComparator::kInstance.evaluate(minKey.Obj() >= maxKey.Obj())) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "min: " << minKey.Obj() << " should be less than max: "
                                  << maxKey.Obj()};
        }

        chunk._min = minKey.Obj().getOwned();
        chunk._max = maxKey.Obj().getOwned();
    }

    {
        std::string chunkShard;
        Status status = bsonExtractStringField(source, shard.name(), &chunkShard);
        if (!status.isOK())
            return status;
        chunk._shard = chunkShard;
    }

    {
        auto statusWithChunkVersion =
            ChunkVersion::parseLegacyWithField(source, ChunkType::lastmod());
        if (!statusWithChunkVersion.isOK()) {
            return statusWithChunkVersion.getStatus();
        }
        auto version = std::move(statusWithChunkVersion.getValue());
        chunk._version = ChunkVersion(version.majorVersion(), version.minorVersion(), epoch);
    }

    {
        BSONElement historyObj;
        Status status = bsonExtractTypedField(source, history.name(), Array, &historyObj);
        if (status.isOK()) {
            auto history = ChunkHistory::fromBSON(BSONArray(historyObj.Obj()));
            if (!history.isOK())
                return history.getStatus();

            chunk._history = std::move(history.getValue());
        } else if (status == ErrorCodes::NoSuchKey) {
            // History is missing, so it will be presumed empty
        } else {
            return status;
        }
    }

    return chunk;
}

BSONObj ChunkType::toShardBSON() const {
    BSONObjBuilder builder;
    invariant(_min);
    invariant(_max);
    invariant(_shard);
    invariant(_version);
    builder.append(minShardID.name(), getMin());
    builder.append(max.name(), getMax());
    builder.append(shard.name(), getShard().toString());
    builder.appendTimestamp(lastmod.name(), _version->toLong());
    addHistoryToBSON(builder);
    return builder.obj();
}

std::string ChunkType::getName() const {
    invariant(_nss);
    invariant(_min);
    return genID(*_nss, *_min);
}

void ChunkType::setNS(const NamespaceString& nss) {
    invariant(nss.isValid());
    _nss = nss;
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

void ChunkType::setShard(const ShardId& shard) {
    invariant(shard.isValid());
    _shard = shard;
}

void ChunkType::setJumbo(bool jumbo) {
    _jumbo = jumbo;
}

void ChunkType::addHistoryToBSON(BSONObjBuilder& builder) const {
    if (_history.size()) {
        BSONArrayBuilder arrayBuilder(builder.subarrayStart(history.name()));
        for (const auto& item : _history) {
            BSONObjBuilder subObjBuilder(arrayBuilder.subobjStart());
            item.serialize(&subObjBuilder);
        }
    }
}

std::string ChunkType::genID(const NamespaceString& nss, const BSONObj& o) {
    StringBuilder buf;
    buf << nss.ns() << "-";

    BSONObjIterator i(o);
    while (i.more()) {
        BSONElement e = i.next();
        buf << e.fieldName() << "_" << e.toString(false, true);
    }

    return buf.str();
}

Status ChunkType::validate() const {
    if (!_min.is_initialized() || _min->isEmpty()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing " << min.name() << " field");
    }

    if (!_max.is_initialized() || _max->isEmpty()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing " << max.name() << " field");
    }

    if (!_version.is_initialized() || !_version->isSet()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing version field");
    }

    if (!_shard.is_initialized() || !_shard->isValid()) {
        return Status(ErrorCodes::NoSuchKey,
                      str::stream() << "missing " << shard.name() << " field");
    }

    // 'min' and 'max' must share the same fields.
    if (_min->nFields() != _max->nFields()) {
        return {ErrorCodes::BadValue,
                str::stream() << "min and max don't have the same number of keys: " << *_min << ", "
                              << *_max};
    }

    BSONObjIterator minIt(getMin());
    BSONObjIterator maxIt(getMax());
    while (minIt.more() && maxIt.more()) {
        BSONElement minElem = minIt.next();
        BSONElement maxElem = maxIt.next();
        if (strcmp(minElem.fieldName(), maxElem.fieldName())) {
            return {ErrorCodes::BadValue,
                    str::stream() << "min and max don't have matching keys: " << *_min << ", "
                                  << *_max};
        }
    }

    // 'max' should be greater than 'min'.
    if (_min->woCompare(getMax()) >= 0) {
        return {ErrorCodes::BadValue,
                str::stream() << "max is not greater than min: " << *_min << ", " << *_max};
    }

    if (!_history.empty()) {
        if (_history.front().getShard() != *_shard) {
            return {ErrorCodes::BadValue,
                    str::stream() << "History contains an invalid shard "
                                  << _history.front().getShard()};
        }
    }

    return Status::OK();
}

std::string ChunkType::toString() const {
    // toConfigBSON will include all the set fields, whereas toShardBSON includes only a subset and
    // requires them to be set.
    return toConfigBSON().toString();
}

}  // namespace mongo
