/*
*    Copyright (C) 2012 10gen Inc.
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

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class AuthorizationManager;
class AuthzSessionExternalState;
class OperationContext;

/**
 * Public interface for a class that encapsulates all the information related to system
 * state not stored in AuthorizationManager.  This is primarily to make AuthorizationManager
 * easier to test as well as to allow different implementations for mongos and mongod.
 */
class AuthzManagerExternalState {
    MONGO_DISALLOW_COPYING(AuthzManagerExternalState);

public:
    static stdx::function<std::unique_ptr<AuthzManagerExternalState>()> create;

    virtual ~AuthzManagerExternalState();

    /**
     * Initializes the external state object.  Must be called after construction and before
     * calling other methods.  Object may not be used after this method returns something other
     * than Status::OK().
     */
    virtual Status initialize(OperationContext* txn) = 0;

    /**
     * Creates an external state manipulator for an AuthorizationSession whose
     * AuthorizationManager uses this object as its own external state manipulator.
     */
    virtual std::unique_ptr<AuthzSessionExternalState> makeAuthzSessionExternalState(
        AuthorizationManager* authzManager) = 0;

    /**
     * Retrieves the schema version of the persistent data describing users and roles.
     * Will leave *outVersion unmodified on non-OK status return values.
     */
    virtual Status getStoredAuthorizationVersion(OperationContext* txn, int* outVersion) = 0;

    /**
     * Writes into "result" a document describing the named user and returns Status::OK().  The
     * description includes the user credentials, if present, the user's role membership and
     * delegation information, a full list of the user's privileges, and a full list of the
     * user's roles, including those roles held implicitly through other roles (indirect roles).
     * In the event that some of this information is inconsistent, the document will contain a
     * "warnings" array, with std::string messages describing inconsistencies.
     *
     * If the user does not exist, returns ErrorCodes::UserNotFound.
     */
    virtual Status getUserDescription(OperationContext* txn,
                                      const UserName& userName,
                                      BSONObj* result) = 0;

    /**
     * Writes into "result" a document describing the named role and returns Status::OK().  The
     * description includes the roles in which the named role has membership and a full list of
     * the roles of which the named role is a member, including those roles memberships held
     * implicitly through other roles (indirect roles). If "showPrivileges" is true, then the
     * description documents will also include a full list of the role's privileges.
     * In the event that some of this information is inconsistent, the document will contain a
     * "warnings" array, with std::string messages describing inconsistencies.
     *
     * If the role does not exist, returns ErrorCodes::RoleNotFound.
     */
    virtual Status getRoleDescription(OperationContext* txn,
                                      const RoleName& roleName,
                                      bool showPrivileges,
                                      BSONObj* result) = 0;

    /**
     * Writes into "result" documents describing the roles that are defined on the given
     * database. Each role description document includes the other roles in which the role has
     * membership and a full list of the roles of which the named role is a member,
     * including those roles memberships held implicitly through other roles (indirect roles).
     * If showPrivileges is true, then the description documents will also include a full list
     * of the role's privileges.  If showBuiltinRoles is true, then the result array will
     * contain description documents for all the builtin roles for the given database, if it
     * is false the result will just include user defined roles.
     * In the event that some of the information in a given role description is inconsistent,
     * the document will contain a "warnings" array, with std::string messages describing
     * inconsistencies.
     */
    virtual Status getRoleDescriptionsForDB(OperationContext* txn,
                                            const std::string dbname,
                                            bool showPrivileges,
                                            bool showBuiltinRoles,
                                            std::vector<BSONObj>* result) = 0;

    /**
     * Returns true if there exists at least one privilege document in the system.
     */
    virtual bool hasAnyPrivilegeDocuments(OperationContext* txn) = 0;

    virtual void logOp(
        OperationContext* txn, const char* op, const char* ns, const BSONObj& o, BSONObj* o2) {}


protected:
    AuthzManagerExternalState();  // This class should never be instantiated directly.
};

}  // namespace mongo
