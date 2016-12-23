/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "string_data.h"

namespace mongo {

/**
 * A StringData::ComparatorInterface is an abstract class for comparing StringData objects.
 */
class StringData::ComparatorInterface {
    MONGO_DISALLOW_COPYING(ComparatorInterface);

public:
    /**
     * Functor for checking string equality under this comparator. Compatible for use with unordered
     * STL containers.
     */
    class EqualTo {
    public:
        explicit EqualTo(const ComparatorInterface* stringComparator)
            : _stringComparator(stringComparator) {}

        bool operator()(StringData lhs, StringData rhs) const {
            return _stringComparator->compare(lhs, rhs) == 0;
        }

    private:
        const ComparatorInterface* _stringComparator;
    };

    /**
     * Functor for hashing strings under this comparator. Compatible for use with unordered STL
     * containers.
     */
    class Hasher {
    public:
        explicit Hasher(const ComparatorInterface* stringComparator)
            : _stringComparator(stringComparator) {}

        size_t operator()(StringData stringToHash) const {
            return _stringComparator->hash(stringToHash);
        }

    private:
        const ComparatorInterface* _stringComparator;
    };

    using StringDataUnorderedSet = stdx::unordered_set<StringData, Hasher, EqualTo>;

    template <typename T>
    using StringDataUnorderedMap = stdx::unordered_map<StringData, T, Hasher, EqualTo>;

    ComparatorInterface() = default;

    virtual ~ComparatorInterface() = default;

    /**
     * Compares two StringData objects.
     */
    virtual int compare(StringData left, StringData right) const = 0;

    /**
     * Hash a StringData in a way that respects this comparator.
     */
    size_t hash(StringData stringToHash) const {
        size_t seed = 0;
        hash_combine(seed, stringToHash);
        return seed;
    }

    /**
     * Hash a StringData in a way that respects this comparator, and return the result in the 'seed'
     * in-out parameter.
     */
    virtual void hash_combine(size_t& seed, StringData stringToHash) const = 0;

    /**
     * Returns a function object which can evaluate string equality according to this comparator.
     * This comparator must outlive the returned function object.
     */
    EqualTo makeEqualTo() const {
        return EqualTo(this);
    }

    /**
     * Returns a function object which can hash strings according to this comparator. This
     * comparator must outlive the returned function object.
     */
    Hasher makeHasher() const {
        return Hasher(this);
    }

    /**
     * Construct an empty unordered set of StringData whose equivalence classes are given by this
     * comparator. This comparator must outlive the returned set.
     */
    StringDataUnorderedSet makeStringDataUnorderedSet() const {
        return StringDataUnorderedSet(0, makeHasher(), makeEqualTo());
    }

    /**
     * Construct an empty unordered map from StringData to type T whose equivalence classes are
     * given by this comparator. This comparator must outlive the returned set.
     */
    template <typename T>
    StringDataUnorderedMap<T> makeStringDataUnorderedMap() const {
        return StringDataUnorderedMap<T>(0, makeHasher(), makeEqualTo());
    }
};

using StringDataUnorderedSet = StringData::ComparatorInterface::StringDataUnorderedSet;

template <typename T>
using StringDataUnorderedMap = StringData::ComparatorInterface::StringDataUnorderedMap<T>;

}  // namespace mongo
