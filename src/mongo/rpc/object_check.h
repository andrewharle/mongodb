/**
 *    Copyright (C) 2015 MongoDB Inc.
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
        // If we're in the primary/master role accepting writes, but our feature compatibility
        // version is 3.2, then we want to reject insertion of the decimal data type. Therefore, we
        // perform BSON 1.0 validation.
        if (serverGlobalParams.featureCompatibility.validateFeaturesAsMaster.load() &&
            serverGlobalParams.featureCompatibility.version.load() ==
                ServerGlobalParams::FeatureCompatibility::Version::k32) {
            return BSONVersion::kV1_0;
        }

        // Except for the special case above, we want to accept any BSON version which we know
        // about. For instance, if we are a slave/secondary syncing from a primary/master and we are
        // in 3.2 feature compatibility mode, we still want to be able to sync NumberDecimal data.
        return BSONVersion::kV1_1;
    }

    inline static Status validateLoad(const char* ptr, size_t length) {
        return serverGlobalParams.objcheck ? validateBSON(ptr, length, enabledBSONVersion())
                                           : Status::OK();
    }

    static Status validateStore(const BSONObj& toStore);
};
}  // namespace mongo
