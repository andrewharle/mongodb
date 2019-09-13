
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
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/operation_context.h"

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class Status;
template <typename T>
class StatusWith;

namespace rpc {

/**
 * This class comprises the request metadata fields involving auditing.
 */
class AuditMetadata {
public:
    static const OperationContext::Decoration<AuditMetadata> get;

    // Decorable requires a default constructor.
    AuditMetadata() = default;

    static StatusWith<AuditMetadata> readFromMetadata(const BSONObj& metadataObj);

    /**
     * Parses AuditMetadata from a pre-extracted BSONElement. When reading a metadata object, this
     * form is more efficient as it permits parsing the metadata in one pass.
     */
    static StatusWith<AuditMetadata> readFromMetadata(const BSONElement& metadataElem);

    Status writeToMetadata(BSONObjBuilder* metadataBob) const;

    using UsersAndRoles = std::tuple<std::vector<UserName>, std::vector<RoleName>>;

    const boost::optional<UsersAndRoles>& getImpersonatedUsersAndRoles() const;

    AuditMetadata(boost::optional<UsersAndRoles> impersonatedUsersAndRoles);

    static StringData fieldName() {
        return "$audit";
    }

private:
    boost::optional<UsersAndRoles> _impersonatedUsersAndRoles;
};

}  // namespace rpc
}  // namespace mongo
