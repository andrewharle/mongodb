
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

#include <boost/optional.hpp>
#include <iosfwd>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/util/container_size_helper.h"

namespace mongo {

/**
 * A FieldPath represents a path in a document, starting from the root. The path
 * is made of "field parts" separated by dots. The class provides an efficient means to
 * "split" the dotted fields in its parts, but no validation is done.
 *
 * Any field part may be replaced, after the "original" field reference was parsed. Any
 * part can be accessed through a StringData object.
 *
 * The class is not thread safe.
 */
class FieldRef {
    MONGO_DISALLOW_COPYING(FieldRef);

public:
    FieldRef();

    explicit FieldRef(StringData path);

    /**
     * Field parts accessed through getPart() calls no longer would be valid, after the destructor
     * ran.
     */
    ~FieldRef() {}

    /**
     * Builds a field path out of each field part in 'dottedField'.
     */
    void parse(StringData dottedField);

    /**
     * Sets the 'i-th' field part to point to 'part'. Assumes i < size(). Behavior is undefined
     * otherwise.
     */
    void setPart(size_t i, StringData part);

    /**
     * Adds a new field to the end of the path, increasing its size by 1.
     */
    void appendPart(StringData part);

    /**
     * Removes the last part from the path, decreasing its size by 1. Has no effect on a FieldRef
     * with size 0.
     */
    void removeLastPart();

    /**
     * Returns the 'i-th' field part. Assumes i < size(). Behavior is undefined otherwise.
     */
    StringData getPart(size_t i) const;

    /**
     * Returns true when 'this' FieldRef is a prefix of 'other'. Equality is not considered
     * a prefix.
     */
    bool isPrefixOf(const FieldRef& other) const;

    /**
     * Returns the number of field parts in the prefix that 'this' and 'other' share.
     */
    size_t commonPrefixSize(const FieldRef& other) const;

    /**
     * Returns a StringData of the full dotted field in its current state (i.e., some parts may
     * have been replaced since the parse() call).
     */
    StringData dottedField(size_t offsetFromStart = 0) const;

    /**
     * Returns a StringData of parts of the dotted field from startPart to endPart in its current
     * state (i.e., some parts may have been replaced since the parse() call).
     */
    StringData dottedSubstring(size_t startPart, size_t endPart) const;

    /**
     * Compares the full dotted path represented by this FieldRef to other
     */
    bool equalsDottedField(StringData other) const;

    /**
     * Return 0 if 'this' is equal to 'other' lexicographically, -1 if is it less than or +1 if it
     * is greater than.
     */
    int compare(const FieldRef& other) const;

    /**
     * Resets the internal state. See note in parse() call.
     */
    void clear();

    //
    // accessors
    //

    /**
     * Returns the number of parts in this FieldRef.
     */
    size_t numParts() const {
        return _size;
    }

    bool empty() const {
        return numParts() == 0;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return  // Add size of each element in '_replacements' vector.
            container_size_helper::estimateObjectSizeInBytes(
                _replacements, [](const std::string& s) { return s.capacity(); }, true) +
            // Add size of each element in '_variable' vector.
            container_size_helper::estimateObjectSizeInBytes(_variable) +
            // Add runtime size of '_dotted' string.
            _dotted.capacity() +
            // Add size of the object.
            sizeof(*this);
    }

private:
    // Dotted fields are most often not longer than four parts. We use a mixed structure
    // here that will not require any extra memory allocation when that is the case. And
    // handle larger dotted fields if it is. The idea is not to penalize the common case
    // with allocations.
    static const size_t kReserveAhead = 4;

    /** Converts the field part index to the variable part equivalent */
    size_t getIndex(size_t i) const {
        return i - kReserveAhead;
    }

    /**
     * Returns the new number of parts after appending 'part' to this field path. This is
     * private, because it is only intended for use by the parse function.
     */
    size_t appendParsedPart(StringData part);

    /**
     * Re-assemble _dotted from components, including any replacements in _replacements,
     * and update the StringData components in _fixed and _variable to refer to the parts
     * of the new _dotted. This is used to make the storage for the current value of this
     * FieldRef contiguous so it can be returned as a StringData from the dottedField
     * method above.
     */
    void reserialize() const;

    // number of field parts stored
    size_t _size;

    // Number of field parts in the cached dotted name (_dotted).
    mutable size_t _cachedSize;

    /**
     * First kResevedAhead field components.
     *
     * Each component is either a StringData backed by the _dotted string or boost::none
     * to indicate that getPart() should read the string from the _replacements list.
     */
    mutable boost::optional<StringData> _fixed[kReserveAhead];

    // Remaining field components. (See comment above _fixed.)
    mutable std::vector<boost::optional<StringData>> _variable;

    /**
     * Cached copy of the complete dotted name string. The StringData objects in "_fixed"
     * and "_variable" reference this string.
     */
    mutable std::string _dotted;

    /**
     * String storage for path parts that have been replaced with setPart() or added with
     * appendPart() since the lasted time "_dotted" was materialized.
     */
    mutable std::vector<std::string> _replacements;
};

inline bool operator==(const FieldRef& lhs, const FieldRef& rhs) {
    return lhs.compare(rhs) == 0;
}

inline bool operator!=(const FieldRef& lhs, const FieldRef& rhs) {
    return lhs.compare(rhs) != 0;
}

inline bool operator<(const FieldRef& lhs, const FieldRef& rhs) {
    return lhs.compare(rhs) < 0;
}

inline bool operator<=(const FieldRef& lhs, const FieldRef& rhs) {
    return lhs.compare(rhs) <= 0;
}

inline bool operator>(const FieldRef& lhs, const FieldRef& rhs) {
    return lhs.compare(rhs) > 0;
}

inline bool operator>=(const FieldRef& lhs, const FieldRef& rhs) {
    return lhs.compare(rhs) >= 0;
}

std::ostream& operator<<(std::ostream& stream, const FieldRef& value);

}  // namespace mongo
