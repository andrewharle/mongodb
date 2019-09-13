// util/base64.h


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

#include <sstream>
#include <string>

#include "mongo/base/string_data.h"

namespace mongo {
namespace base64 {

void encode(std::stringstream& ss, const char* data, int size);
std::string encode(const char* data, int size);
std::string encode(const std::string& s);

void decode(std::stringstream& ss, const std::string& s);
std::string decode(const std::string& s);

bool validate(StringData);

/**
 * Calculate how large a given input would expand to.
 * Effectively: ceil(inLen * 4 / 3)
 */
constexpr size_t encodedLength(size_t inLen) {
    return static_cast<size_t>((inLen + 2.5) / 3) * 4;
}

}  // namespace base64
}  // namespace mongo
