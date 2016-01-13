/*    Copyright 2013 10gen Inc.

 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/db/auth/user.h"

#include <vector>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/sequence_util.h"

namespace mongo {

    User::User(const UserName& name) :
        _name(name),
        _schemaVersion(AuthorizationManager::schemaVersion26Final),
        _refCount(0),
        _isValid(1) {}

    User::~User() {
        dassert(_refCount == 0);
    }

    const UserName& User::getName() const {
        return _name;
    }

    RoleNameIterator User::getRoles() const {
        return makeRoleNameIteratorForContainer(_roles);
    }

    RoleNameIterator User::getIndirectRoles() const {
         return makeRoleNameIteratorForContainer(_indirectRoles);
    }

    bool User::hasRole(const RoleName& roleName) const {
        return _roles.count(roleName);
    }

    const User::CredentialData& User::getCredentials() const {
        return _credentials;
    }

    bool User::isValid() const {
        return _isValid.loadRelaxed() == 1;
    }

    uint32_t User::getRefCount() const {
        return _refCount;
    }

    const ActionSet User::getActionsForResource(const ResourcePattern& resource) const {
        unordered_map<ResourcePattern, Privilege>::const_iterator it = _privileges.find(resource);
        if (it == _privileges.end()) {
            return ActionSet();
        }
        return it->second.getActions();
    }

    User* User::clone() const {
        std::auto_ptr<User> result(new User(_name));
        result->_privileges = _privileges;
        result->_roles = _roles;
        result->_probedDatabases = _probedDatabases;
        result->_credentials = _credentials;
        result->_schemaVersion = _schemaVersion;
        return result.release();
    }

    void User::setCredentials(const CredentialData& credentials) {
        _credentials = credentials;
    }

    void User::setRoles(RoleNameIterator roles) {
        _roles.clear();
        while (roles.more()) {
            _roles.insert(roles.next());
        }
    }

    void User::setIndirectRoles(RoleNameIterator indirectRoles) {
        _indirectRoles.clear();
        while (indirectRoles.more()) {
            _indirectRoles.push_back(indirectRoles.next());
        }
    }

    void User::setPrivileges(const PrivilegeVector& privileges) {
        _privileges.clear();
        for (size_t i = 0; i < privileges.size(); ++i) {
            const Privilege& privilege = privileges[i];
            _privileges[privilege.getResourcePattern()] = privilege;
        }
    }

    void User::addRole(const RoleName& roleName) {
        _roles.insert(roleName);
    }

    void User::addRoles(const std::vector<RoleName>& roles) {
        for (std::vector<RoleName>::const_iterator it = roles.begin(); it != roles.end(); ++it) {
            addRole(*it);
        }
    }

    void User::addPrivilege(const Privilege& privilegeToAdd) {
        ResourcePrivilegeMap::iterator it = _privileges.find(privilegeToAdd.getResourcePattern());
        if (it == _privileges.end()) {
            // No privilege exists yet for this resource
            _privileges.insert(std::make_pair(privilegeToAdd.getResourcePattern(), privilegeToAdd));
        } else {
            dassert(it->first == privilegeToAdd.getResourcePattern());
            it->second.addActions(privilegeToAdd.getActions());
        }
    }

    void User::addPrivileges(const PrivilegeVector& privileges) {
        for (PrivilegeVector::const_iterator it = privileges.begin();
                it != privileges.end(); ++it) {
            addPrivilege(*it);
        }
    }

    void User::setSchemaVersion1() {
        _schemaVersion = AuthorizationManager::schemaVersion24;
    }

    void User::markProbedV1(const StringData& dbname) {
        dassert(_schemaVersion == AuthorizationManager::schemaVersion24);
        if (!hasProbedV1(dbname))
            _probedDatabases.push_back(dbname.toString());
    }

    bool User::hasProbedV1(const StringData& dbname) const {
        dassert(_schemaVersion == AuthorizationManager::schemaVersion24);
        return sequenceContains(_probedDatabases, dbname);
    }

    void User::invalidate() {
        _isValid.store(0);
    }

    void User::incrementRefCount() {
        ++_refCount;
    }

    void User::decrementRefCount() {
        dassert(_refCount > 0);
        --_refCount;
    }
} // namespace mongo
