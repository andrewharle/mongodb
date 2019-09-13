
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

#include "mongo/rpc/object_check.h"

#include "mongo/base/status.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/db/server_parameters.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/stringutils.h"

namespace mongo {
namespace {

MONGO_COMPILER_VARIABLE_UNUSED auto _exportedMaxBSONDepth =
    (new ExportedServerParameter<std::int32_t, ServerParameterType::kStartupOnly>(
        ServerParameterSet::getGlobal(), "maxBSONDepth", &BSONDepth::maxAllowableDepth))
        -> withValidator([](const std::int32_t& potentialNewValue) {
            if (potentialNewValue < BSONDepth::kBSONDepthParameterFloor ||
                potentialNewValue > BSONDepth::kBSONDepthParameterCeiling) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "maxBSONDepth must be between "
                                            << BSONDepth::kBSONDepthParameterFloor
                                            << " and "
                                            << BSONDepth::kBSONDepthParameterCeiling
                                            << ", inclusive");
            }
            return Status::OK();
        });

}  // namespace

Status Validator<BSONObj>::validateStore(const BSONObj& toStore) {
    return Status::OK();
}
}  // namespace mongo
