
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

#include "mongo/db/auth/role_graph.h"

#include <algorithm>
#include <set>
#include <vector>

#include "mongo/bson/mutable/document.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
PrivilegeVector emptyPrivilegeVector;
}  // namespace

void RoleGraph::swap(RoleGraph& other) {
    using std::swap;
    swap(this->_roleToSubordinates, other._roleToSubordinates);
    swap(this->_roleToIndirectSubordinates, other._roleToIndirectSubordinates);
    swap(this->_roleToMembers, other._roleToMembers);
    swap(this->_directPrivilegesForRole, other._directPrivilegesForRole);
    swap(this->_allPrivilegesForRole, other._allPrivilegesForRole);
    swap(this->_directRestrictionsForRole, other._directRestrictionsForRole);
    swap(this->_allRestrictionsForRole, other._allRestrictionsForRole);
    swap(this->_allRoles, other._allRoles);
}

bool RoleGraph::roleExists(const RoleName& role) {
    _createBuiltinRoleIfNeeded(role);
    return _roleExistsDontCreateBuiltin(role);
}

bool RoleGraph::_roleExistsDontCreateBuiltin(const RoleName& role) {
    EdgeSet::const_iterator edgeIt = _roleToSubordinates.find(role);
    if (edgeIt == _roleToSubordinates.end())
        return false;
    edgeIt = _roleToMembers.find(role);
    fassert(16825, edgeIt != _roleToMembers.end());

    RolePrivilegeMap::const_iterator strIt = _directPrivilegesForRole.find(role);
    if (strIt == _directPrivilegesForRole.end())
        return false;
    strIt = _allPrivilegesForRole.find(role);
    fassert(16826, strIt != _allPrivilegesForRole.end());
    return true;
}

Status RoleGraph::createRole(const RoleName& role) {
    if (roleExists(role)) {
        return Status(ErrorCodes::DuplicateKey,
                      mongoutils::str::stream() << "Role: " << role.getFullName()
                                                << " already exists");
    }

    _createRoleDontCheckIfRoleExists(role);
    return Status::OK();
}

void RoleGraph::_createRoleDontCheckIfRoleExists(const RoleName& role) {
    // Just reference the role in all the maps so that an entry gets created with empty
    // containers for the value.
    _roleToSubordinates[role];
    _roleToIndirectSubordinates[role];
    _roleToMembers[role];
    _directPrivilegesForRole[role];
    _allPrivilegesForRole[role];
    _allRoles.insert(role);
}

Status RoleGraph::deleteRole(const RoleName& role) {
    if (!roleExists(role)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << "Role: " << role.getFullName()
                                                << " does not exist");
    }
    if (isBuiltinRole(role)) {
        return Status(ErrorCodes::InvalidRoleModification,
                      mongoutils::str::stream() << "Cannot delete built-in role: "
                                                << role.getFullName());
    }

    for (std::vector<RoleName>::iterator it = _roleToSubordinates[role].begin();
         it != _roleToSubordinates[role].end();
         ++it) {
        _roleToMembers[*it].erase(
            std::find(_roleToMembers[*it].begin(), _roleToMembers[*it].end(), role));
    }
    for (std::vector<RoleName>::iterator it = _roleToMembers[role].begin();
         it != _roleToMembers[role].end();
         ++it) {
        _roleToSubordinates[*it].erase(
            std::find(_roleToSubordinates[*it].begin(), _roleToSubordinates[*it].end(), role));
    }
    _roleToSubordinates.erase(role);
    _roleToIndirectSubordinates.erase(role);
    _roleToMembers.erase(role);
    _directPrivilegesForRole.erase(role);
    _allPrivilegesForRole.erase(role);
    _allRoles.erase(role);
    return Status::OK();
}

RoleNameIterator RoleGraph::getDirectSubordinates(const RoleName& role) {
    if (!roleExists(role))
        return RoleNameIterator(NULL);
    return makeRoleNameIteratorForContainer(_roleToSubordinates[role]);
}

RoleNameIterator RoleGraph::getIndirectSubordinates(const RoleName& role) {
    if (!roleExists(role))
        return RoleNameIterator(NULL);
    return makeRoleNameIteratorForContainer(_roleToIndirectSubordinates[role]);
}

RoleNameIterator RoleGraph::getDirectMembers(const RoleName& role) {
    if (!roleExists(role))
        return RoleNameIterator(NULL);
    return makeRoleNameIteratorForContainer(_roleToMembers[role]);
}

const PrivilegeVector& RoleGraph::getDirectPrivileges(const RoleName& role) {
    if (!roleExists(role))
        return emptyPrivilegeVector;
    return _directPrivilegesForRole.find(role)->second;
}

const PrivilegeVector& RoleGraph::getAllPrivileges(const RoleName& role) {
    if (!roleExists(role))
        return emptyPrivilegeVector;
    return _allPrivilegesForRole.find(role)->second;
}

Status RoleGraph::addRoleToRole(const RoleName& recipient, const RoleName& role) {
    if (!roleExists(recipient)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << "Role: " << recipient.getFullName()
                                                << " does not exist");
    }
    if (isBuiltinRole(recipient)) {
        return Status(ErrorCodes::InvalidRoleModification,
                      mongoutils::str::stream() << "Cannot grant roles to built-in role: "
                                                << role.getFullName());
    }
    if (!roleExists(role)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << "Role: " << role.getFullName()
                                                << " does not exist");
    }

    if (std::find(_roleToSubordinates[recipient].begin(),
                  _roleToSubordinates[recipient].end(),
                  role) == _roleToSubordinates[recipient].end()) {
        // Only add role if it's not already present
        _roleToSubordinates[recipient].push_back(role);
        _roleToMembers[role].push_back(recipient);
    }

    return Status::OK();
}

Status RoleGraph::removeRoleFromRole(const RoleName& recipient, const RoleName& role) {
    if (!roleExists(recipient)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << "Role: " << recipient.getFullName()
                                                << " does not exist");
    }
    if (isBuiltinRole(recipient)) {
        return Status(ErrorCodes::InvalidRoleModification,
                      mongoutils::str::stream() << "Cannot remove roles from built-in role: "
                                                << role.getFullName());
    }
    if (!roleExists(role)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << "Role: " << role.getFullName()
                                                << " does not exist");
    }

    std::vector<RoleName>::iterator itToRm =
        std::find(_roleToMembers[role].begin(), _roleToMembers[role].end(), recipient);
    if (itToRm != _roleToMembers[role].end()) {
        _roleToMembers[role].erase(itToRm);
    } else {
        return Status(ErrorCodes::RolesNotRelated,
                      mongoutils::str::stream() << recipient.getFullName() << " is not a member"
                                                                              " of "
                                                << role.getFullName());
    }

    itToRm = std::find(
        _roleToSubordinates[recipient].begin(), _roleToSubordinates[recipient].end(), role);
    fassert(16827, itToRm != _roleToSubordinates[recipient].end());
    _roleToSubordinates[recipient].erase(itToRm);
    return Status::OK();
}

Status RoleGraph::removeAllRolesFromRole(const RoleName& victim) {
    typedef std::vector<RoleName> RoleNameVector;
    if (!roleExists(victim)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << "Role: " << victim.getFullName()
                                                << " does not exist");
    }
    if (isBuiltinRole(victim)) {
        return Status(ErrorCodes::InvalidRoleModification,
                      mongoutils::str::stream() << "Cannot remove roles from built-in role: "
                                                << victim.getFullName());
    }

    RoleNameVector& subordinatesOfVictim = _roleToSubordinates[victim];
    for (RoleNameVector::const_iterator subordinateRole = subordinatesOfVictim.begin(),
                                        end = subordinatesOfVictim.end();
         subordinateRole != end;
         ++subordinateRole) {
        RoleNameVector& membersOfSubordinate = _roleToMembers[*subordinateRole];
        RoleNameVector::iterator toErase =
            std::find(membersOfSubordinate.begin(), membersOfSubordinate.end(), victim);
        fassert(17173, toErase != membersOfSubordinate.end());
        membersOfSubordinate.erase(toErase);
    }
    subordinatesOfVictim.clear();
    return Status::OK();
}

Status RoleGraph::addPrivilegeToRole(const RoleName& role, const Privilege& privilegeToAdd) {
    if (!roleExists(role)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << "Role: " << role.getFullName()
                                                << " does not exist");
    }
    if (isBuiltinRole(role)) {
        return Status(ErrorCodes::InvalidRoleModification,
                      mongoutils::str::stream() << "Cannot grant privileges to built-in role: "
                                                << role.getFullName());
    }

    _addPrivilegeToRoleNoChecks(role, privilegeToAdd);
    return Status::OK();
}

void RoleGraph::_addPrivilegeToRoleNoChecks(const RoleName& role, const Privilege& privilegeToAdd) {
    Privilege::addPrivilegeToPrivilegeVector(&_directPrivilegesForRole[role], privilegeToAdd);
}

// NOTE: Current runtime of this is O(n*m) where n is the size of the current PrivilegeVector
// for the given role, and m is the size of the privilegesToAdd vector.
// If this was a PrivilegeSet (sorted on resource) rather than a PrivilegeVector, we
// could do this in O(n+m) instead.
Status RoleGraph::addPrivilegesToRole(const RoleName& role,
                                      const PrivilegeVector& privilegesToAdd) {
    if (!roleExists(role)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << "Role: " << role.getFullName()
                                                << " does not exist");
    }
    if (isBuiltinRole(role)) {
        return Status(ErrorCodes::InvalidRoleModification,
                      mongoutils::str::stream() << "Cannot grant privileges to built-in role: "
                                                << role.getFullName());
    }

    for (PrivilegeVector::const_iterator it = privilegesToAdd.begin(); it != privilegesToAdd.end();
         ++it) {
        _addPrivilegeToRoleNoChecks(role, *it);
    }
    return Status::OK();
}

Status RoleGraph::removePrivilegeFromRole(const RoleName& role,
                                          const Privilege& privilegeToRemove) {
    if (!roleExists(role)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << "Role: " << role.getFullName()
                                                << " does not exist");
    }
    if (isBuiltinRole(role)) {
        return Status(ErrorCodes::InvalidRoleModification,
                      mongoutils::str::stream() << "Cannot remove privileges from built-in role: "
                                                << role.getFullName());
    }

    PrivilegeVector& currentPrivileges = _directPrivilegesForRole[role];
    for (PrivilegeVector::iterator it = currentPrivileges.begin(); it != currentPrivileges.end();
         ++it) {
        Privilege& curPrivilege = *it;
        if (curPrivilege.getResourcePattern() == privilegeToRemove.getResourcePattern()) {
            ActionSet curActions = curPrivilege.getActions();

            if (!curActions.isSupersetOf(privilegeToRemove.getActions())) {
                // Didn't possess all the actions being removed.
                return Status(
                    ErrorCodes::PrivilegeNotFound,
                    mongoutils::str::stream() << "Role: " << role.getFullName()
                                              << " does not contain a privilege on "
                                              << privilegeToRemove.getResourcePattern().toString()
                                              << " with actions: "
                                              << privilegeToRemove.getActions().toString());
            }

            curPrivilege.removeActions(privilegeToRemove.getActions());
            if (curPrivilege.getActions().empty()) {
                currentPrivileges.erase(it);
            }
            return Status::OK();
        }
    }
    return Status(ErrorCodes::PrivilegeNotFound,
                  mongoutils::str::stream() << "Role: " << role.getFullName()
                                            << " does not "
                                               "contain any privileges on "
                                            << privilegeToRemove.getResourcePattern().toString());
}

Status RoleGraph::removePrivilegesFromRole(const RoleName& role,
                                           const PrivilegeVector& privilegesToRemove) {
    for (PrivilegeVector::const_iterator it = privilegesToRemove.begin();
         it != privilegesToRemove.end();
         ++it) {
        Status status = removePrivilegeFromRole(role, *it);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status RoleGraph::removeAllPrivilegesFromRole(const RoleName& role) {
    if (!roleExists(role)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << "Role: " << role.getFullName()
                                                << " does not exist");
    }
    if (isBuiltinRole(role)) {
        return Status(ErrorCodes::InvalidRoleModification,
                      mongoutils::str::stream() << "Cannot remove privileges from built-in role: "
                                                << role.getFullName());
    }
    _directPrivilegesForRole[role].clear();
    return Status::OK();
}

Status RoleGraph::replaceRestrictionsForRole(const RoleName& role,
                                             SharedRestrictionDocument restrictions) {
    if (!roleExists(role)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << "Role: " << role.getFullName()
                                                << " does not exist");
    }
    if (isBuiltinRole(role)) {
        return Status(ErrorCodes::InvalidRoleModification,
                      mongoutils::str::stream() << "Cannot remove restrictions from built-in role: "
                                                << role.getFullName());
    }
    _directRestrictionsForRole[role] = std::move(restrictions);
    return Status::OK();
}

Status RoleGraph::replaceRole(const RoleName& roleName,
                              const std::vector<RoleName>& roles,
                              const PrivilegeVector& privileges,
                              SharedRestrictionDocument restrictions) {
    Status status = removeAllPrivilegesFromRole(roleName);
    if (status == ErrorCodes::RoleNotFound) {
        fassert(17168, createRole(roleName));
    } else if (!status.isOK()) {
        return status;
    }
    fassert(17169, removeAllRolesFromRole(roleName));
    fassert(40556, replaceRestrictionsForRole(roleName, restrictions));
    for (size_t i = 0; i < roles.size(); ++i) {
        const RoleName& grantedRole = roles[i];
        status = createRole(grantedRole);
        fassert(17170, status.isOK() || status == ErrorCodes::DuplicateKey);
        fassert(17171, addRoleToRole(roleName, grantedRole));
    }
    fassert(17172, addPrivilegesToRole(roleName, privileges));
    return Status::OK();
}

Status RoleGraph::recomputePrivilegeData() {
    /*
     * This method is used to recompute the "allPrivileges" vector for each node in the graph,
     * as well as look for cycles.  It is implemented by performing a depth-first traversal of
     * the dependency graph, once for each node.  "visitedRoles" tracks the set of role names
     * ever visited, and it is used to prune each DFS.  A node that has been visited once on any
     * DFS is never visited again.  Complexity of this implementation is O(n+m) where "n" is the
     * number of nodes and "m" is the number of prerequisite edges.  Space complexity is O(n),
     * in both stack space and size of the "visitedRoles" set.
     *
     * "inProgressRoles" is used to detect and report cycles, as well as to keep track of roles
     * we started visiting before realizing they had children that needed visiting first, so
     * we can get back to them after visiting their children.
     */

    stdx::unordered_set<RoleName> visitedRoles;
    for (EdgeSet::const_iterator it = _roleToSubordinates.begin(); it != _roleToSubordinates.end();
         ++it) {
        Status status = _recomputePrivilegeDataHelper(it->first, visitedRoles);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status RoleGraph::_recomputePrivilegeDataHelper(const RoleName& startingRole,
                                                stdx::unordered_set<RoleName>& visitedRoles) {
    if (visitedRoles.count(startingRole)) {
        return Status::OK();
    }

    std::vector<RoleName> inProgressRoles;
    inProgressRoles.push_back(startingRole);
    while (inProgressRoles.size()) {
        const RoleName currentRole = inProgressRoles.back();
        fassert(17277, !visitedRoles.count(currentRole));

        if (!roleExists(currentRole)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role: " << currentRole.getFullName()
                                                    << " does not exist");
        }

        // Check for cycles
        {
            const std::vector<RoleName>::const_iterator begin = inProgressRoles.begin();
            // The currentRole will always be last so don't look there.
            const std::vector<RoleName>::const_iterator end = --inProgressRoles.end();
            const std::vector<RoleName>::const_iterator firstOccurence =
                std::find(begin, end, currentRole);
            if (firstOccurence != end) {
                std::ostringstream os;
                os << "Cycle in dependency graph: ";
                for (std::vector<RoleName>::const_iterator it = firstOccurence; it != end; ++it) {
                    os << it->getFullName() << " -> ";
                }
                os << currentRole.getFullName();
                return Status(ErrorCodes::GraphContainsCycle, os.str());
            }
        }

        // Make sure we've already visited all subordinate roles before worrying about this one.
        const std::vector<RoleName>& currentRoleDirectRoles = _roleToSubordinates[currentRole];
        std::vector<RoleName>::const_iterator roleIt;
        for (roleIt = currentRoleDirectRoles.begin(); roleIt != currentRoleDirectRoles.end();
             ++roleIt) {
            const RoleName& childRole = *roleIt;
            if (!visitedRoles.count(childRole)) {
                inProgressRoles.push_back(childRole);
                break;
            }
        }
        // If roleIt didn't reach the end of currentRoleDirectRoles that means we found a child
        // of currentRole that we haven't visited yet.
        if (roleIt != currentRoleDirectRoles.end()) {
            continue;
        }
        // At this point, we know that we've already visited all child roles of currentRole
        // and thus their "all privileges" sets are correct and can be added to currentRole's
        // "all privileges" set

        // Need to clear out the "all privileges" vector for the current role, and re-fill it
        // with just the direct privileges for this role.
        PrivilegeVector& currentRoleAllPrivileges = _allPrivilegesForRole[currentRole];
        currentRoleAllPrivileges = _directPrivilegesForRole[currentRole];

        // Need to do the same thing for the indirect roles
        stdx::unordered_set<RoleName>& currentRoleIndirectRoles =
            _roleToIndirectSubordinates[currentRole];
        currentRoleIndirectRoles.clear();
        for (const auto& role : currentRoleDirectRoles) {
            currentRoleIndirectRoles.insert(role);
        }

        // Also clear the "all restrictions" to rebuild in loop
        auto& currentRoleAllRestrictions = _allRestrictionsForRole[currentRole];
        currentRoleAllRestrictions.clear();
        auto& currentRoleDirectRestrictions = _directRestrictionsForRole[currentRole];
        if (currentRoleDirectRestrictions) {
            currentRoleAllRestrictions.push_back(currentRoleDirectRestrictions);
        }

        // Recursively add children's privileges to current role's "all privileges" vector, and
        // children's roles to current roles's "indirect roles" vector.
        for (const auto& childRole : currentRoleDirectRoles) {
            // At this point, we already know that the "all privilege" set for the child is
            // correct, so add those privileges to our "all privilege" set.

            for (const auto& priv : _allPrivilegesForRole[childRole]) {
                Privilege::addPrivilegeToPrivilegeVector(&currentRoleAllPrivileges, priv);
            }

            // We also know that the "indirect roles" for the child is also correct, so we can
            // add those roles to our "indirect roles" set.
            const auto& childAllRolesToIndirectSubordinates =
                _roleToIndirectSubordinates[childRole];
            currentRoleIndirectRoles.insert(childAllRolesToIndirectSubordinates.begin(),
                                            childAllRolesToIndirectSubordinates.end());

            // Similarly, "indirect restrictions" are ready to append
            const auto& childAllRestrictionsForRole = _allRestrictionsForRole[childRole];
            currentRoleAllRestrictions.insert(currentRoleAllRestrictions.end(),
                                              childAllRestrictionsForRole.begin(),
                                              childAllRestrictionsForRole.end());
        }

        visitedRoles.insert(currentRole);
        inProgressRoles.pop_back();
    }
    return Status::OK();
}

RoleNameIterator RoleGraph::getRolesForDatabase(const std::string& dbname) {
    _createBuiltinRolesForDBIfNeeded(dbname);

    std::set<RoleName>::const_iterator lower = _allRoles.lower_bound(RoleName("", dbname));
    std::string afterDB = dbname;
    afterDB.push_back('\0');
    std::set<RoleName>::const_iterator upper = _allRoles.lower_bound(RoleName("", afterDB));
    return makeRoleNameIterator(lower, upper);
}


Status RoleGraph::getBSONForRole(RoleGraph* graph,
                                 const RoleName& roleName,
                                 mutablebson::Element result) try {
    if (!graph->roleExists(roleName)) {
        return Status(ErrorCodes::RoleNotFound,
                      mongoutils::str::stream() << roleName.getFullName()
                                                << "does not name an existing role");
    }
    std::string id = mongoutils::str::stream() << roleName.getDB() << "." << roleName.getRole();
    uassertStatusOK(result.appendString("_id", id));
    uassertStatusOK(
        result.appendString(AuthorizationManager::ROLE_NAME_FIELD_NAME, roleName.getRole()));
    uassertStatusOK(
        result.appendString(AuthorizationManager::ROLE_DB_FIELD_NAME, roleName.getDB()));

    // Build privileges array
    mutablebson::Element privilegesArrayElement =
        result.getDocument().makeElementArray("privileges");
    uassertStatusOK(result.pushBack(privilegesArrayElement));
    const PrivilegeVector& privileges = graph->getDirectPrivileges(roleName);
    uassertStatusOK(Privilege::getBSONForPrivileges(privileges, privilegesArrayElement));

    // Build roles array
    mutablebson::Element rolesArrayElement = result.getDocument().makeElementArray("roles");
    uassertStatusOK(result.pushBack(rolesArrayElement));
    for (RoleNameIterator roles = graph->getDirectSubordinates(roleName); roles.more();
         roles.next()) {
        const RoleName& subRole = roles.get();
        mutablebson::Element roleObj = result.getDocument().makeElementObject("");
        uassertStatusOK(
            roleObj.appendString(AuthorizationManager::ROLE_NAME_FIELD_NAME, subRole.getRole()));
        uassertStatusOK(
            roleObj.appendString(AuthorizationManager::ROLE_DB_FIELD_NAME, subRole.getDB()));
        uassertStatusOK(rolesArrayElement.pushBack(roleObj));
    }

    return Status::OK();
} catch (...) {
    return exceptionToStatus();
}

}  // namespace mongo
