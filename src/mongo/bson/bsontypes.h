// bsontypes.h


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

#include <iosfwd>

#include "mongo/config.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BSONArrayBuilder;
class BSONElement;
class BSONElementCmpWithoutField;
class BSONObj;
class BSONObjBuilder;
class BSONObjBuilderValueStream;
class BSONObjIterator;
class Ordering;
struct BSONArray;  // empty subclass of BSONObj useful for overloading

extern const BSONObj kMaxBSONKey;
extern const BSONObj kMinBSONKey;

/**
    determines BSON types considered valid by validate
*/
enum class BSONVersion { kV1_0, kV1_1, kLatest = kV1_1 };

/**
    the complete list of valid BSON types
    see also bsonspec.org
*/
enum BSONType {
    /** smaller than all other types */
    MinKey = -1,
    /** end of object */
    EOO = 0,
    /** double precision floating point value */
    NumberDouble = 1,
    /** character string, stored in utf8 */
    String = 2,
    /** an embedded object */
    Object = 3,
    /** an embedded array */
    Array = 4,
    /** binary data */
    BinData = 5,
    /** Undefined type */
    Undefined = 6,
    /** ObjectId */
    jstOID = 7,
    /** boolean type */
    Bool = 8,
    /** date type */
    Date = 9,
    /** null type */
    jstNULL = 10,
    /** regular expression, a pattern with options */
    RegEx = 11,
    /** deprecated / will be redesigned */
    DBRef = 12,
    /** deprecated / use CodeWScope */
    Code = 13,
    /** a programming language (e.g., Python) symbol */
    Symbol = 14,
    /** javascript code that can execute on the database server, with SavedContext */
    CodeWScope = 15,
    /** 32 bit signed integer */
    NumberInt = 16,
    /** Two 32 bit signed integers */
    bsonTimestamp = 17,
    /** 64 bit integer */
    NumberLong = 18,
    /** 128 bit decimal */
    NumberDecimal = 19,
    /** max type that is not MaxKey */
    JSTypeMax = 19,
    /** larger than all other types */
    MaxKey = 127
};

/**
 * Maps from the set of type aliases accepted by the $type query operator to the corresponding BSON
 * types. Excludes "number", since this alias maps to a set of BSON types.
 */
extern const StringMap<BSONType> kTypeAliasMap;

/**
 * returns the name of the argument's type
 */
const char* typeName(BSONType type);

/**
 * Reverse mapping of typeName(). Throws an exception with error code BadValue when passed in
 * invalid type name.
 */
BSONType typeFromName(StringData name);

/**
 * Prints the name of the argument's type to the given stream.
 */
std::ostream& operator<<(std::ostream& stream, BSONType type);

/**
 * Returns whether or not 'type' can be converted to a valid BSONType.
 */
bool isValidBSONType(int type);

inline bool isNumericBSONType(BSONType type) {
    switch (type) {
        case NumberDouble:
        case NumberInt:
        case NumberLong:
        case NumberDecimal:
            return true;
        default:
            return false;
    }
}

/* subtypes of BinData.
   bdtCustom and above are ones that the JS compiler understands, but are
   opaque to the database.
*/
enum BinDataType {
    BinDataGeneral = 0,
    Function = 1,
    ByteArrayDeprecated = 2, /* use BinGeneral instead */
    bdtUUID = 3,             /* deprecated */
    newUUID = 4,             /* language-independent UUID format across all drivers */
    MD5Type = 5,
    bdtCustom = 128
};

/**
 * Return the name of the BinData Type.
 */
const char* typeName(BinDataType type);

/** Returns a number for where a given type falls in the sort order.
 *  Elements with the same return value should be compared for value equality.
 *  The return value is not a BSONType and should not be treated as one.
 *  Note: if the order changes, indexes have to be re-built or than can be corruption
 */
inline int canonicalizeBSONType(BSONType type) {
    switch (type) {
        case MinKey:
        case MaxKey:
            return type;
        case EOO:
        case Undefined:
            return 0;
        case jstNULL:
            return 5;
        case NumberDecimal:
        case NumberDouble:
        case NumberInt:
        case NumberLong:
            return 10;
        case mongo::String:
        case Symbol:
            return 15;
        case Object:
            return 20;
        case mongo::Array:
            return 25;
        case BinData:
            return 30;
        case jstOID:
            return 35;
        case mongo::Bool:
            return 40;
        case mongo::Date:
            return 45;
        case bsonTimestamp:
            return 47;
        case RegEx:
            return 50;
        case DBRef:
            return 55;
        case Code:
            return 60;
        case CodeWScope:
            return 65;
        default:
            verify(0);
            return -1;
    }
}
}
