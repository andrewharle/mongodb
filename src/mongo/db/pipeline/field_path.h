/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * Utility class which represents a field path with nested paths separated by dots.
 */
class FieldPath {
public:
    /**
     * Throws a UserException if a field name does not pass validation.
     */
    static void uassertValidFieldName(StringData fieldName);

    /**
     * Concatenates 'prefix' and 'suffix' using dotted path notation. 'prefix' is allowed to be
     * empty.
     */
    static std::string getFullyQualifiedPath(StringData prefix, StringData suffix);

    /**
     * Returns the substring of 'path' until the first '.', or the entire string if there is no '.'.
     */
    static StringData extractFirstFieldFromDottedPath(StringData path) {
        return path.substr(0, path.find('.'));
    }

    /**
     * Throws a UserException if the string is empty or if any of the field names fail validation.
     *
     * Field names are validated using uassertValidFieldName().
     */
    /* implicit */ FieldPath(std::string inputPath);
    /* implicit */ FieldPath(StringData inputPath) : FieldPath(inputPath.toString()) {}
    /* implicit */ FieldPath(const char* inputPath) : FieldPath(std::string(inputPath)) {}

    /**
     * Returns the number of path elements in the field path.
     */
    size_t getPathLength() const {
        return _fieldPathDotPosition.size() - 1;
    }

    /**
     * Return the ith field name from this path using zero-based indexes.
     */
    StringData getFieldName(size_t i) const {
        dassert(i < getPathLength());
        const auto begin = _fieldPathDotPosition[i] + 1;
        const auto end = _fieldPathDotPosition[i + 1];
        return StringData(&_fieldPath[begin], end - begin);
    }

    /**
     * Returns the full path, not including the prefix 'FieldPath::prefix'.
     */
    const std::string& fullPath() const {
        return _fieldPath;
    }

    /**
     * Returns the full path, including the prefix 'FieldPath::prefix'.
     */
    std::string fullPathWithPrefix() const {
        return prefix + _fieldPath;
    }
    /**
     * A FieldPath like this but missing the first element (useful for recursion).
     * Precondition getPathLength() > 1.
     */
    FieldPath tail() const {
        massert(16409, "FieldPath::tail() called on single element path", getPathLength() > 1);
        return {_fieldPath.substr(_fieldPathDotPosition[1] + 1)};
    }

private:
    static const char prefix = '$';

    // Contains the full field path, with each field delimited by a '.' character.
    std::string _fieldPath;

    // Contains the position of field delimiter dots in '_fieldPath'. The first element contains
    // string::npos (which evaluates to -1) and the last contains _fieldPath.size() to facilitate
    // lookup.
    std::vector<size_t> _fieldPathDotPosition;
};
}
