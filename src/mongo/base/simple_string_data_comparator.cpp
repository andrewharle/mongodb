
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

#include "mongo/base/simple_string_data_comparator.h"

#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"

namespace mongo {

namespace {

template <int SizeofSizeT>
size_t murmur3(StringData str, size_t seed);

template <>
size_t murmur3<4>(StringData str, size_t seed) {
    char hash[4];
    MurmurHash3_x86_32(str.rawData(), str.size(), seed, &hash);
    return ConstDataView(hash).read<LittleEndian<std::uint32_t>>();
}

template <>
size_t murmur3<8>(StringData str, size_t seed) {
    char hash[16];
    MurmurHash3_x64_128(str.rawData(), str.size(), seed, hash);
    return static_cast<size_t>(ConstDataView(hash).read<LittleEndian<std::uint64_t>>());
}

}  // namespace

const SimpleStringDataComparator SimpleStringDataComparator::kInstance{};

int SimpleStringDataComparator::compare(StringData left, StringData right) const {
    return left.compare(right);
}

void SimpleStringDataComparator::hash_combine(size_t& seed, StringData stringToHash) const {
    seed = murmur3<sizeof(size_t)>(stringToHash, seed);
}

}  // namespace mongo
