
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

#include "mongo/base/data_type_validated.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/server_options.h"

// We do not use the rpc namespace here so we can specialize Validator.
namespace mongo {
class BSONObj;
class Status;

/**
 * A validator for BSON objects. The implementation will validate the input object
 * if validation is enabled, or return Status::OK() otherwise.
 */
template <>
struct Validator<BSONObj> {
    inline static BSONVersion enabledBSONVersion() {
        // The enabled BSON version is always the latest BSON version if no new BSON types have been
        // added during the release. Otherwise, the BSON version returned should be controlled
        // through the featureCompatibilityVersion.
        return BSONVersion::kLatest;
    }

    inline static Status validateLoad(const char* ptr, size_t length) {
        return serverGlobalParams.objcheck ? validateBSON(ptr, length, enabledBSONVersion())
                                           : Status::OK();
    }

    static Status validateStore(const BSONObj& toStore);
};
}  // namespace mongo
