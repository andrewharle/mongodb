
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

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

namespace mutablebson {
class Document;
}  // namespace mutablebson

/**
 * Common implementation of AuthzManagerExternalState for systems where role
 * and user information are stored locally.
 */
class AuthzManagerExternalStateLocal : public AuthzManagerExternalState {
    MONGO_DISALLOW_COPYING(AuthzManagerExternalStateLocal);

public:
    virtual ~AuthzManagerExternalStateLocal() = default;

    Status initialize(OperationContext* opCtx) override;

    Status getStoredAuthorizationVersion(OperationContext* opCtx, int* outVersion) override;
    Status getUserDescription(OperationContext* opCtx,
                              const UserName& userName,
                              BSONObj* result) override;
    Status getRoleDescription(OperationContext* opCtx,
                              const RoleName& roleName,
                              PrivilegeFormat showPrivileges,
                              AuthenticationRestrictionsFormat,
                              BSONObj* result) override;
    Status getRolesDescription(OperationContext* opCtx,
                               const std::vector<RoleName>& roles,
                               PrivilegeFormat showPrivileges,
                               AuthenticationRestrictionsFormat,
                               BSONObj* result) override;
    Status getRoleDescriptionsForDB(OperationContext* opCtx,
                                    const std::string& dbname,
                                    PrivilegeFormat showPrivileges,
                                    AuthenticationRestrictionsFormat,
                                    bool showBuiltinRoles,
                                    std::vector<BSONObj>* result) override;

    bool hasAnyPrivilegeDocuments(OperationContext* opCtx) override;

    /**
     * Finds a document matching "query" in "collectionName", and store a shared-ownership
     * copy into "result".
     *
     * Returns Status::OK() on success.  If no match is found, returns
     * ErrorCodes::NoMatchingDocument.  Other errors returned as appropriate.
     */
    virtual Status findOne(OperationContext* opCtx,
                           const NamespaceString& collectionName,
                           const BSONObj& query,
                           BSONObj* result) = 0;

    /**
     * Finds all documents matching "query" in "collectionName".  For each document returned,
     * calls the function resultProcessor on it.
     */
    virtual Status query(OperationContext* opCtx,
                         const NamespaceString& collectionName,
                         const BSONObj& query,
                         const BSONObj& projection,
                         const stdx::function<void(const BSONObj&)>& resultProcessor) = 0;

    virtual void logOp(OperationContext* opCtx,
                       const char* op,
                       const NamespaceString& ns,
                       const BSONObj& o,
                       const BSONObj* o2);

    /**
     * Takes a user document, and processes it with the RoleGraph, in order to recursively
     * resolve roles and add the 'inheritedRoles', 'inheritedPrivileges',
     * and 'warnings' fields.
     */
    void resolveUserRoles(mutablebson::Document* userDoc, const std::vector<RoleName>& directRoles);

protected:
    AuthzManagerExternalStateLocal() = default;

private:
    enum RoleGraphState {
        roleGraphStateInitial = 0,
        roleGraphStateConsistent,
        roleGraphStateHasCycle
    };

    /**
     * RecoveryUnit::Change subclass used to commit work for AuthzManager logOp listener.
     */
    class AuthzManagerLogOpHandler;

    /**
     * Initializes the role graph from the contents of the admin.system.roles collection.
     */
    Status _initializeRoleGraph(OperationContext* opCtx);

    /**
     * Fetches the user document for "userName" from local storage, and stores it into "result".
     */
    Status _getUserDocument(OperationContext* opCtx, const UserName& userName, BSONObj* result);

    Status _getRoleDescription_inlock(const RoleName& roleName,
                                      PrivilegeFormat showPrivileges,
                                      AuthenticationRestrictionsFormat showRestrictions,
                                      BSONObj* result);
    /**
     * Eventually consistent, in-memory representation of all roles in the system (both
     * user-defined and built-in).  Synchronized via _roleGraphMutex.
     */
    RoleGraph _roleGraph;

    /**
     * State of _roleGraph, one of "initial", "consistent" and "has cycle".  Synchronized via
     * _roleGraphMutex.
     */
    RoleGraphState _roleGraphState = roleGraphStateInitial;

    /**
     * Guards _roleGraphState and _roleGraph.
     */
    stdx::mutex _roleGraphMutex;
};

}  // namespace mongo
