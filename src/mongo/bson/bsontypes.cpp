/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/bson/bsontypes.h"

#include "mongo/config.h"
#include "mongo/db/jsobj.h"

namespace mongo {

const char kMaxKeyData[] = {7, 0, 0, 0, static_cast<char>(MaxKey), 0, 0};
const BSONObj kMaxBSONKey(kMaxKeyData);

const char kMinKeyData[] = {7, 0, 0, 0, static_cast<char>(MinKey), 0, 0};
const BSONObj kMinBSONKey(kMinKeyData);

/* take a BSONType and return the name of that type as a char* */
const char* typeName(BSONType type) {
    switch (type) {
        case MinKey:
            return "MinKey";
        case EOO:
            return "EOO";
        case NumberDouble:
            return "NumberDouble";
        case String:
            return "String";
        case Object:
            return "Object";
        case Array:
            return "Array";
        case BinData:
            return "BinaryData";
        case Undefined:
            return "Undefined";
        case jstOID:
            return "OID";
        case Bool:
            return "Bool";
        case Date:
            return "Date";
        case jstNULL:
            return "NULL";
        case RegEx:
            return "RegEx";
        case DBRef:
            return "DBRef";
        case Code:
            return "Code";
        case Symbol:
            return "Symbol";
        case CodeWScope:
            return "CodeWScope";
        case NumberInt:
            return "NumberInt32";
        case bsonTimestamp:
            return "Timestamp";
        case NumberLong:
            return "NumberLong64";
        case NumberDecimal:
            return "NumberDecimal128";
        // JSTypeMax doesn't make sense to turn into a string; overlaps with highest-valued type
        case MaxKey:
            return "MaxKey";
        default:
            return "Invalid";
    }
}

bool isValidBSONType(int type) {
    switch (type) {
        case MinKey:
        case EOO:
        case NumberDouble:
        case String:
        case Object:
        case Array:
        case BinData:
        case Undefined:
        case jstOID:
        case Bool:
        case Date:
        case jstNULL:
        case RegEx:
        case DBRef:
        case Code:
        case Symbol:
        case CodeWScope:
        case NumberInt:
        case bsonTimestamp:
        case NumberLong:
#ifdef MONGO_CONFIG_EXPERIMENTAL_DECIMAL_SUPPORT
        case NumberDecimal:
#endif
        case MaxKey:
            return true;
        default:
            return false;
    }
}

}  // namespace mongo
