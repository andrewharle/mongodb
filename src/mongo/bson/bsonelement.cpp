
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/bson/bsonelement.h"

#include <boost/functional/hash.hpp>
#include <cmath>

#include "mongo/base/compare_numbers.h"
#include "mongo/base/data_cursor.h"
#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/strnlen.h"
#include "mongo/util/base64.h"
#include "mongo/util/duration.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace str = mongoutils::str;

using std::dec;
using std::hex;
using std::string;

const double BSONElement::kLongLongMaxPlusOneAsDouble =
    scalbn(1, std::numeric_limits<long long>::digits);

string BSONElement::jsonString(JsonStringFormat format, bool includeFieldNames, int pretty) const {
    std::stringstream s;
    if (includeFieldNames)
        s << '"' << escape(fieldName()) << "\" : ";
    switch (type()) {
        case mongo::String:
        case Symbol:
            s << '"' << escape(string(valuestr(), valuestrsize() - 1)) << '"';
            break;
        case NumberLong:
            if (format == TenGen) {
                s << "NumberLong(" << _numberLong() << ")";
            } else {
                s << "{ \"$numberLong\" : \"" << _numberLong() << "\" }";
            }
            break;
        case NumberInt:
            if (format == TenGen) {
                s << "NumberInt(" << _numberInt() << ")";
                break;
            }
        case NumberDouble:
            if (number() >= -std::numeric_limits<double>::max() &&
                number() <= std::numeric_limits<double>::max()) {
                s.precision(16);
                s << number();
            }
            // This is not valid JSON, but according to RFC-4627, "Numeric values that cannot be
            // represented as sequences of digits (such as Infinity and NaN) are not permitted." so
            // we are accepting the fact that if we have such values we cannot output valid JSON.
            else if (std::isnan(number())) {
                s << "NaN";
            } else if (std::isinf(number())) {
                s << (number() > 0 ? "Infinity" : "-Infinity");
            } else {
                StringBuilder ss;
                ss << "Number " << number() << " cannot be represented in JSON";
                string message = ss.str();
                massert(10311, message.c_str(), false);
            }
            break;
        case NumberDecimal:
            if (format == TenGen)
                s << "NumberDecimal(\"";
            else
                s << "{ \"$numberDecimal\" : \"";
            // Recognize again that this is not valid JSON according to RFC-4627.
            // Also, treat -NaN and +NaN as the same thing for MongoDB.
            if (numberDecimal().isNaN()) {
                s << "NaN";
            } else if (numberDecimal().isInfinite()) {
                s << (numberDecimal().isNegative() ? "-Infinity" : "Infinity");
            } else {
                s << numberDecimal().toString();
            }
            if (format == TenGen)
                s << "\")";
            else
                s << "\" }";
            break;
        case mongo::Bool:
            s << (boolean() ? "true" : "false");
            break;
        case jstNULL:
            s << "null";
            break;
        case Undefined:
            if (format == Strict) {
                s << "{ \"$undefined\" : true }";
            } else {
                s << "undefined";
            }
            break;
        case Object:
            s << embeddedObject().jsonString(format, pretty);
            break;
        case mongo::Array: {
            if (embeddedObject().isEmpty()) {
                s << "[]";
                break;
            }
            s << "[ ";
            BSONObjIterator i(embeddedObject());
            BSONElement e = i.next();
            if (!e.eoo()) {
                int count = 0;
                while (1) {
                    if (pretty) {
                        s << '\n';
                        for (int x = 0; x < pretty; x++)
                            s << "  ";
                    }

                    if (strtol(e.fieldName(), 0, 10) > count) {
                        s << "undefined";
                    } else {
                        s << e.jsonString(format, false, pretty ? pretty + 1 : 0);
                        e = i.next();
                    }
                    count++;
                    if (e.eoo())
                        break;
                    s << ", ";
                }
            }
            s << " ]";
            break;
        }
        case DBRef: {
            if (format == TenGen)
                s << "Dbref( ";
            else
                s << "{ \"$ref\" : ";
            s << '"' << valuestr() << "\", ";
            if (format != TenGen)
                s << "\"$id\" : ";
            s << '"' << mongo::OID::from(valuestr() + valuestrsize()) << "\" ";
            if (format == TenGen)
                s << ')';
            else
                s << '}';
            break;
        }
        case jstOID:
            if (format == TenGen) {
                s << "ObjectId( ";
            } else {
                s << "{ \"$oid\" : ";
            }
            s << '"' << __oid() << '"';
            if (format == TenGen) {
                s << " )";
            } else {
                s << " }";
            }
            break;
        case BinData: {
            ConstDataCursor reader(value());
            const int len = reader.readAndAdvance<LittleEndian<int>>();
            BinDataType type = static_cast<BinDataType>(reader.readAndAdvance<uint8_t>());

            s << "{ \"$binary\" : \"";
            base64::encode(s, reader.view(), len);
            s << "\", \"$type\" : \"" << hex;
            s.width(2);
            s.fill('0');
            s << type << dec;
            s << "\" }";
            break;
        }
        case mongo::Date:
            if (format == Strict) {
                Date_t d = date();
                s << "{ \"$date\" : ";
                // The two cases in which we cannot convert Date_t::millis to an ISO Date string are
                // when the date is too large to format (SERVER-13760), and when the date is before
                // the epoch (SERVER-11273).  Since Date_t internally stores millis as an unsigned
                // long long, despite the fact that it is logically signed (SERVER-8573), this check
                // handles both the case where Date_t::millis is too large, and the case where
                // Date_t::millis is negative (before the epoch).
                if (d.isFormattable()) {
                    s << "\"" << dateToISOStringLocal(date()) << "\"";
                } else {
                    s << "{ \"$numberLong\" : \"" << d.toMillisSinceEpoch() << "\" }";
                }
                s << " }";
            } else {
                s << "Date( ";
                if (pretty) {
                    Date_t d = date();
                    // The two cases in which we cannot convert Date_t::millis to an ISO Date string
                    // are when the date is too large to format (SERVER-13760), and when the date is
                    // before the epoch (SERVER-11273).  Since Date_t internally stores millis as an
                    // unsigned long long, despite the fact that it is logically signed
                    // (SERVER-8573), this check handles both the case where Date_t::millis is too
                    // large, and the case where Date_t::millis is negative (before the epoch).
                    if (d.isFormattable()) {
                        s << "\"" << dateToISOStringLocal(date()) << "\"";
                    } else {
                        // FIXME: This is not parseable by the shell, since it may not fit in a
                        // float
                        s << d.toMillisSinceEpoch();
                    }
                } else {
                    s << date().asInt64();
                }
                s << " )";
            }
            break;
        case RegEx:
            if (format == Strict) {
                s << "{ \"$regex\" : \"" << escape(regex());
                s << "\", \"$options\" : \"" << regexFlags() << "\" }";
            } else {
                s << "/" << escape(regex(), true) << "/";
                // FIXME Worry about alpha order?
                for (const char* f = regexFlags(); *f; ++f) {
                    switch (*f) {
                        case 'g':
                        case 'i':
                        case 'm':
                            s << *f;
                        default:
                            break;
                    }
                }
            }
            break;

        case CodeWScope: {
            BSONObj scope = codeWScopeObject();
            if (!scope.isEmpty()) {
                s << "{ \"$code\" : \"" << escape(_asCode()) << "\" , "
                  << "\"$scope\" : " << scope.jsonString() << " }";
                break;
            }
        }

        case Code:
            s << "\"" << escape(_asCode()) << "\"";
            break;

        case bsonTimestamp:
            if (format == TenGen) {
                s << "Timestamp( " << durationCount<Seconds>(timestampTime().toDurationSinceEpoch())
                  << ", " << timestampInc() << " )";
            } else {
                s << "{ \"$timestamp\" : { \"t\" : "
                  << durationCount<Seconds>(timestampTime().toDurationSinceEpoch())
                  << ", \"i\" : " << timestampInc() << " } }";
            }
            break;

        case MinKey:
            s << "{ \"$minKey\" : 1 }";
            break;

        case MaxKey:
            s << "{ \"$maxKey\" : 1 }";
            break;

        default:
            StringBuilder ss;
            ss << "Cannot create a properly formatted JSON string with "
               << "element: " << toString() << " of type: " << type();
            string message = ss.str();
            massert(10312, message.c_str(), false);
    }
    return s.str();
}

namespace {

// Compares two string elements using a simple binary compare.
int compareElementStringValues(const BSONElement& leftStr, const BSONElement& rightStr) {
    // we use memcmp as we allow zeros in UTF8 strings
    int lsz = leftStr.valuestrsize();
    int rsz = rightStr.valuestrsize();
    int common = std::min(lsz, rsz);
    int res = memcmp(leftStr.valuestr(), rightStr.valuestr(), common);
    if (res)
        return res;
    // longer std::string is the greater one
    return lsz - rsz;
}

}  // namespace

int BSONElement::compareElements(const BSONElement& l,
                                 const BSONElement& r,
                                 ComparisonRulesSet rules,
                                 const StringData::ComparatorInterface* comparator) {
    switch (l.type()) {
        case BSONType::EOO:
        case BSONType::Undefined:  // EOO and Undefined are same canonicalType
        case BSONType::jstNULL:
        case BSONType::MaxKey:
        case BSONType::MinKey: {
            auto f = l.canonicalType() - r.canonicalType();
            if (f < 0)
                return -1;
            return f == 0 ? 0 : 1;
        }
        case BSONType::Bool:
            return *l.value() - *r.value();
        case BSONType::bsonTimestamp:
            // unsigned compare for timestamps - note they are not really dates but (ordinal +
            // time_t)
            if (l.timestamp() < r.timestamp())
                return -1;
            return l.timestamp() == r.timestamp() ? 0 : 1;
        case BSONType::Date:
            // Signed comparisons for Dates.
            {
                const Date_t a = l.Date();
                const Date_t b = r.Date();
                if (a < b)
                    return -1;
                return a == b ? 0 : 1;
            }

        case BSONType::NumberInt: {
            // All types can precisely represent all NumberInts, so it is safe to simply convert to
            // whatever rhs's type is.
            switch (r.type()) {
                case NumberInt:
                    return compareInts(l._numberInt(), r._numberInt());
                case NumberLong:
                    return compareLongs(l._numberInt(), r._numberLong());
                case NumberDouble:
                    return compareDoubles(l._numberInt(), r._numberDouble());
                case NumberDecimal:
                    return compareIntToDecimal(l._numberInt(), r._numberDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::NumberLong: {
            switch (r.type()) {
                case NumberLong:
                    return compareLongs(l._numberLong(), r._numberLong());
                case NumberInt:
                    return compareLongs(l._numberLong(), r._numberInt());
                case NumberDouble:
                    return compareLongToDouble(l._numberLong(), r._numberDouble());
                case NumberDecimal:
                    return compareLongToDecimal(l._numberLong(), r._numberDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::NumberDouble: {
            switch (r.type()) {
                case NumberDouble:
                    return compareDoubles(l._numberDouble(), r._numberDouble());
                case NumberInt:
                    return compareDoubles(l._numberDouble(), r._numberInt());
                case NumberLong:
                    return compareDoubleToLong(l._numberDouble(), r._numberLong());
                case NumberDecimal:
                    return compareDoubleToDecimal(l._numberDouble(), r._numberDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::NumberDecimal: {
            switch (r.type()) {
                case NumberDecimal:
                    return compareDecimals(l._numberDecimal(), r._numberDecimal());
                case NumberInt:
                    return compareDecimalToInt(l._numberDecimal(), r._numberInt());
                case NumberLong:
                    return compareDecimalToLong(l._numberDecimal(), r._numberLong());
                case NumberDouble:
                    return compareDecimalToDouble(l._numberDecimal(), r._numberDouble());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case BSONType::jstOID:
            return memcmp(l.value(), r.value(), OID::kOIDSize);
        case BSONType::Code:
            return compareElementStringValues(l, r);
        case BSONType::Symbol:
        case BSONType::String: {
            if (comparator) {
                return comparator->compare(l.valueStringData(), r.valueStringData());
            } else {
                return compareElementStringValues(l, r);
            }
        }
        case BSONType::Object:
        case BSONType::Array: {
            return l.embeddedObject().woCompare(
                r.embeddedObject(),
                BSONObj(),
                rules | BSONElement::ComparisonRules::kConsiderFieldName,
                comparator);
        }
        case BSONType::DBRef: {
            int lsz = l.valuesize();
            int rsz = r.valuesize();
            if (lsz - rsz != 0)
                return lsz - rsz;
            return memcmp(l.value(), r.value(), lsz);
        }
        case BSONType::BinData: {
            int lsz = l.objsize();  // our bin data size in bytes, not including the subtype byte
            int rsz = r.objsize();
            if (lsz - rsz != 0)
                return lsz - rsz;
            return memcmp(l.value() + 4, r.value() + 4, lsz + 1 /*+1 for subtype byte*/);
        }
        case BSONType::RegEx: {
            int c = strcmp(l.regex(), r.regex());
            if (c)
                return c;
            return strcmp(l.regexFlags(), r.regexFlags());
        }
        case BSONType::CodeWScope: {
            int cmp = StringData(l.codeWScopeCode(), l.codeWScopeCodeLen() - 1)
                          .compare(StringData(r.codeWScopeCode(), r.codeWScopeCodeLen() - 1));
            if (cmp)
                return cmp;

            // When comparing the scope object, we should consider field names. Special string
            // comparison semantics do not apply to strings nested inside the CodeWScope scope
            // object, so we do not pass through the string comparator.
            return l.codeWScopeObject().woCompare(
                r.codeWScopeObject(),
                BSONObj(),
                rules | BSONElement::ComparisonRules::kConsiderFieldName);
        }
    }

    MONGO_UNREACHABLE;
}

/** transform a BSON array into a vector of BSONElements.
    we match array # positions with their vector position, and ignore
    any fields with non-numeric field names.
    */
std::vector<BSONElement> BSONElement::Array() const {
    chk(mongo::Array);
    std::vector<BSONElement> v;
    BSONObjIterator i(Obj());
    while (i.more()) {
        BSONElement e = i.next();
        const char* f = e.fieldName();

        unsigned u;
        Status status = parseNumberFromString(f, &u);
        if (status.isOK()) {
            verify(u < 1000000);
            if (u >= v.size())
                v.resize(u + 1);
            v[u] = e;
        } else {
            // ignore?
        }
    }
    return v;
}

int BSONElement::woCompare(const BSONElement& elem,
                           ComparisonRulesSet rules,
                           const StringData::ComparatorInterface* comparator) const {
    if (type() != elem.type()) {
        int lt = (int)canonicalType();
        int rt = (int)elem.canonicalType();
        if (int diff = lt - rt)
            return diff;
    }
    if (rules & ComparisonRules::kConsiderFieldName) {
        if (int diff = fieldNameStringData().compare(elem.fieldNameStringData()))
            return diff;
    }
    return compareElements(*this, elem, rules, comparator);
}

bool BSONElement::binaryEqual(const BSONElement& rhs) const {
    const int elemSize = size();

    if (elemSize != rhs.size()) {
        return false;
    }

    return (elemSize == 0) || (memcmp(data, rhs.rawdata(), elemSize) == 0);
}

bool BSONElement::binaryEqualValues(const BSONElement& rhs) const {
    // The binaryEqual method above implicitly compares the type, but we need to do so explicitly
    // here. It doesn't make sense to consider to BSONElement objects as binaryEqual if they have
    // the same bit pattern but different types (consider an integer and a double).
    if (type() != rhs.type())
        return false;

    const int valueSize = valuesize();
    if (valueSize != rhs.valuesize()) {
        return false;
    }

    return (valueSize == 0) || (memcmp(value(), rhs.value(), valueSize) == 0);
}

BSONObj BSONElement::embeddedObjectUserCheck() const {
    if (MONGO_likely(isABSONObj()))
        return BSONObj(value(), BSONObj::LargeSizeTrait{});
    std::stringstream ss;
    ss << "invalid parameter: expected an object (" << fieldName() << ")";
    uasserted(10065, ss.str());
    return BSONObj();  // never reachable
}

BSONObj BSONElement::embeddedObject() const {
    verify(isABSONObj());
    return BSONObj(value(), BSONObj::LargeSizeTrait{});
}

BSONObj BSONElement::codeWScopeObject() const {
    verify(type() == CodeWScope);
    int strSizeWNull = ConstDataView(value() + 4).read<LittleEndian<int>>();
    return BSONObj(value() + 4 + 4 + strSizeWNull);
}

// wrap this element up as a singleton object.
BSONObj BSONElement::wrap() const {
    BSONObjBuilder b(size() + 6);
    b.append(*this);
    return b.obj();
}

BSONObj BSONElement::wrap(StringData newName) const {
    BSONObjBuilder b(size() + 6 + newName.size());
    b.appendAs(*this, newName);
    return b.obj();
}

void BSONElement::Val(BSONObj& v) const {
    v = Obj();
}

BSONObj BSONElement::Obj() const {
    return embeddedObjectUserCheck();
}

BSONElement BSONElement::operator[](StringData field) const {
    BSONObj o = Obj();
    return o[field];
}

namespace {
NOINLINE_DECL void msgAssertedBadType[[noreturn]](int8_t type) {
    msgasserted(10320, str::stream() << "BSONElement: bad type " << (int)type);
}
}  // namespace
int BSONElement::computeSize() const {
    enum SizeStyle : uint8_t {
        kFixed,         // Total size is a fixed amount + key length.
        kIntPlusFixed,  // Like Fixed, but also add in the int32 immediately following the key.
        kRegEx,         // Handled specially.
    };
    struct SizeInfo {
        uint8_t style : 2;
        uint8_t bytes : 6;  // Includes type byte. Excludes field name and variable lengths.
    };
    MONGO_STATIC_ASSERT(sizeof(SizeInfo) == 1);

    // This table should take 20 bytes. Align to next power of 2 to avoid splitting across cache
    // lines unnecessarily.
    static constexpr SizeInfo kSizeInfoTable alignas(32)[] = {
        {SizeStyle::kFixed, 1},          // EOO
        {SizeStyle::kFixed, 9},          // NumberDouble
        {SizeStyle::kIntPlusFixed, 5},   // String
        {SizeStyle::kIntPlusFixed, 1},   // Object
        {SizeStyle::kIntPlusFixed, 1},   // Array
        {SizeStyle::kIntPlusFixed, 6},   // BinData
        {SizeStyle::kFixed, 1},          // Undefined
        {SizeStyle::kFixed, 13},         // OID
        {SizeStyle::kFixed, 2},          // Bool
        {SizeStyle::kFixed, 9},          // Date
        {SizeStyle::kFixed, 1},          // Null
        {SizeStyle::kRegEx},             // Regex
        {SizeStyle::kIntPlusFixed, 17},  // DBRef
        {SizeStyle::kIntPlusFixed, 5},   // Code
        {SizeStyle::kIntPlusFixed, 5},   // Symbol
        {SizeStyle::kIntPlusFixed, 1},   // CodeWScope
        {SizeStyle::kFixed, 5},          // Int
        {SizeStyle::kFixed, 9},          // Timestamp
        {SizeStyle::kFixed, 9},          // Long
        {SizeStyle::kFixed, 17},         // Decimal
    };
    MONGO_STATIC_ASSERT((sizeof(kSizeInfoTable) / sizeof(kSizeInfoTable[0])) == JSTypeMax + 1);

    // This is the start of the runtime code for this function. Everything above happens at compile
    // time. This function attempts to push complex handling of unlikely events out-of-line to
    // ensure that the common cases never need to spill any registers (at least on x64 with
    // gcc-5.4), which reduces the function call overhead.
    int8_t type = *data;
    if (MONGO_unlikely(type < 0 || type > JSTypeMax)) {
        if (MONGO_unlikely(type != MinKey && type != MaxKey)) {
            msgAssertedBadType(type);
        }

        // MinKey and MaxKey should be treated the same as Null
        type = jstNULL;
    }

    const auto sizeInfo = kSizeInfoTable[type];
    if (sizeInfo.style == SizeStyle::kFixed)
        return sizeInfo.bytes + fieldNameSize();
    if (MONGO_likely(sizeInfo.style == SizeStyle::kIntPlusFixed))
        return sizeInfo.bytes + fieldNameSize() + valuestrsize();

    return [this, type]() NOINLINE_DECL {
        // Regex is two c-strings back-to-back.
        invariant(type == BSONType::RegEx);
        const char* p = value();
        size_t len1 = strlen(p);
        p = p + len1 + 1;
        size_t len2 = strlen(p);
        return (len1 + 1 + len2 + 1) + fieldNameSize() + 1;
    }();
}

std::string BSONElement::toString(bool includeFieldName, bool full) const {
    StringBuilder s;
    toString(s, includeFieldName, full, false);
    return s.str();
}

void BSONElement::toString(
    StringBuilder& s, bool includeFieldName, bool full, bool redactValues, int depth) const {
    if (depth > BSONObj::maxToStringRecursionDepth) {
        // check if we want the full/complete string
        if (full) {
            StringBuilder s;
            s << "Reached maximum recursion depth of ";
            s << BSONObj::maxToStringRecursionDepth;
            uassert(16150, s.str(), full != true);
        }
        s << "...";
        return;
    }

    if (includeFieldName && type() != EOO)
        s << fieldName() << ": ";

    switch (type()) {
        case Object:
            return embeddedObject().toString(s, false, full, redactValues, depth + 1);
        case mongo::Array:
            return embeddedObject().toString(s, true, full, redactValues, depth + 1);
        default:
            break;
    }

    if (redactValues) {
        s << "\"###\"";
        return;
    }

    switch (type()) {
        case EOO:
            s << "EOO";
            break;
        case mongo::Date:
            s << "new Date(" << date().toMillisSinceEpoch() << ')';
            break;
        case RegEx: {
            s << "/" << regex() << '/';
            const char* p = regexFlags();
            if (p)
                s << p;
        } break;
        case NumberDouble:
            s.appendDoubleNice(number());
            break;
        case NumberLong:
            s << _numberLong();
            break;
        case NumberInt:
            s << _numberInt();
            break;
        case NumberDecimal:
            s << _numberDecimal().toString();
            break;
        case mongo::Bool:
            s << (boolean() ? "true" : "false");
            break;
        case Undefined:
            s << "undefined";
            break;
        case jstNULL:
            s << "null";
            break;
        case MaxKey:
            s << "MaxKey";
            break;
        case MinKey:
            s << "MinKey";
            break;
        case CodeWScope:
            s << "CodeWScope( " << codeWScopeCode() << ", " << codeWScopeObject().toString() << ")";
            break;
        case Code:
            if (!full && valuestrsize() > 80) {
                s.write(valuestr(), 70);
                s << "...";
            } else {
                s.write(valuestr(), valuestrsize() - 1);
            }
            break;
        case Symbol:
        case mongo::String:
            s << '"';
            if (!full && valuestrsize() > 160) {
                s.write(valuestr(), 150);
                s << "...\"";
            } else {
                s.write(valuestr(), valuestrsize() - 1);
                s << '"';
            }
            break;
        case DBRef:
            s << "DBRef('" << valuestr() << "',";
            s << mongo::OID::from(valuestr() + valuestrsize()) << ')';
            break;
        case jstOID:
            s << "ObjectId('";
            s << __oid() << "')";
            break;
        case BinData: {
            int len;
            const char* data = binDataClean(len);
            // If the BinData is a correctly sized newUUID, display it as such.
            if (binDataType() == newUUID && len == 16) {
                // 4 Octets - 2 Octets - 2 Octets - 2 Octets - 6 Octets
                s << "UUID(\"";
                s << toHexLower(&data[0], 4);
                s << "-";
                s << toHexLower(&data[4], 2);
                s << "-";
                s << toHexLower(&data[6], 2);
                s << "-";
                s << toHexLower(&data[8], 2);
                s << "-";
                s << toHexLower(&data[10], 6);
                s << "\")";
                break;
            }
            s << "BinData(" << binDataType() << ", ";
            if (!full && len > 80) {
                s << toHex(data, 70) << "...)";
            } else {
                s << toHex(data, len) << ")";
            }
        } break;

        case bsonTimestamp: {
            // Convert from Milliseconds to Seconds for consistent Timestamp printing.
            auto secs = duration_cast<Seconds>(timestampTime().toDurationSinceEpoch());
            s << "Timestamp(" << secs.count() << ", " << timestampInc() << ")";
        } break;
        default:
            s << "?type=" << type();
            break;
    }
}

std::string BSONElement::_asCode() const {
    switch (type()) {
        case mongo::String:
        case Code:
            return std::string(valuestr(), valuestrsize() - 1);
        case CodeWScope:
            return std::string(codeWScopeCode(),
                               ConstDataView(valuestr()).read<LittleEndian<int>>() - 1);
        default:
            log() << "can't convert type: " << (int)(type()) << " to code" << std::endl;
    }
    uassert(10062, "not code", 0);
    return "";
}

std::ostream& operator<<(std::ostream& s, const BSONElement& e) {
    return s << e.toString();
}

StringBuilder& operator<<(StringBuilder& s, const BSONElement& e) {
    e.toString(s);
    return s;
}

template <>
bool BSONElement::coerce<std::string>(std::string* out) const {
    if (type() != mongo::String)
        return false;
    *out = String();
    return true;
}

template <>
bool BSONElement::coerce<int>(int* out) const {
    if (!isNumber())
        return false;
    *out = numberInt();
    return true;
}

template <>
bool BSONElement::coerce<long long>(long long* out) const {
    if (!isNumber())
        return false;
    *out = numberLong();
    return true;
}

template <>
bool BSONElement::coerce<double>(double* out) const {
    if (!isNumber())
        return false;
    *out = numberDouble();
    return true;
}

template <>
bool BSONElement::coerce<Decimal128>(Decimal128* out) const {
    if (!isNumber())
        return false;
    *out = numberDecimal();
    return true;
}

template <>
bool BSONElement::coerce<bool>(bool* out) const {
    *out = trueValue();
    return true;
}

template <>
bool BSONElement::coerce<std::vector<std::string>>(std::vector<std::string>* out) const {
    if (type() != mongo::Array)
        return false;
    return Obj().coerceVector<std::string>(out);
}

template <typename T>
bool BSONObj::coerceVector(std::vector<T>* out) const {
    BSONObjIterator i(*this);
    while (i.more()) {
        BSONElement e = i.next();
        T t;
        if (!e.coerce<T>(&t))
            return false;
        out->push_back(t);
    }
    return true;
}

}  // namespace mongo
