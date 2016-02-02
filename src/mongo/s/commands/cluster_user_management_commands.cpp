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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/commands/user_management_commands.h"

#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/config.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

namespace mongo {

using std::string;
using std::stringstream;
using std::vector;

class CmdCreateUser : public Command {
public:
    CmdCreateUser() : Command("createUser") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Adds a user to the system";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForCreateUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        return grid.catalogManager(txn)
            ->runUserManagementWriteCommand(txn, this->name, dbname, cmdObj, &result);
    }

    virtual void redactForLogging(mutablebson::Document* cmdObj) {
        auth::redactPasswordData(cmdObj->root());
    }

} cmdCreateUser;

class CmdUpdateUser : public Command {
public:
    CmdUpdateUser() : Command("updateUser") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Used to update a user, for example to change its password";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForUpdateUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        auth::CreateOrUpdateUserArgs args;
        Status status = auth::parseCreateOrUpdateUserCommands(cmdObj, this->name, dbname, &args);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserByName(args.userName);

        return ok;
    }

    virtual void redactForLogging(mutablebson::Document* cmdObj) {
        auth::redactPasswordData(cmdObj->root());
    }

} cmdUpdateUser;

class CmdDropUser : public Command {
public:
    CmdDropUser() : Command("dropUser") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Drops a single user.";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForDropUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        UserName userName;
        BSONObj unusedWriteConcern;
        Status status =
            auth::parseAndValidateDropUserCommand(cmdObj, dbname, &userName, &unusedWriteConcern);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserByName(userName);

        return ok;
    }

} cmdDropUser;

class CmdDropAllUsersFromDatabase : public Command {
public:
    CmdDropAllUsersFromDatabase() : Command("dropAllUsersFromDatabase") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Drops all users for a single database.";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForDropAllUsersFromDatabaseCommand(client, dbname);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUsersFromDB(dbname);

        return ok;
    }

} cmdDropAllUsersFromDatabase;

class CmdGrantRolesToUser : public Command {
public:
    CmdGrantRolesToUser() : Command("grantRolesToUser") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Grants roles to a user.";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForGrantRolesToUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        string userNameString;
        vector<RoleName> roles;
        BSONObj unusedWriteConcern;
        Status status = auth::parseRolePossessionManipulationCommands(
            cmdObj, this->name, dbname, &userNameString, &roles, &unusedWriteConcern);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserByName(UserName(userNameString, dbname));

        return ok;
    }

} cmdGrantRolesToUser;

class CmdRevokeRolesFromUser : public Command {
public:
    CmdRevokeRolesFromUser() : Command("revokeRolesFromUser") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Revokes roles from a user.";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForRevokeRolesFromUserCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        string userNameString;
        vector<RoleName> unusedRoles;
        BSONObj unusedWriteConcern;
        Status status = auth::parseRolePossessionManipulationCommands(
            cmdObj, this->name, dbname, &userNameString, &unusedRoles, &unusedWriteConcern);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserByName(UserName(userNameString, dbname));

        return ok;
    }

} cmdRevokeRolesFromUser;

class CmdUsersInfo : public Command {
public:
    virtual bool slaveOk() const {
        return false;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    CmdUsersInfo() : Command("usersInfo") {}

    virtual void help(stringstream& ss) const {
        ss << "Returns information about users.";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForUsersInfoCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        return grid.catalogManager(txn)->runUserManagementReadCommand(txn, dbname, cmdObj, &result);
    }

} cmdUsersInfo;

class CmdCreateRole : public Command {
public:
    CmdCreateRole() : Command("createRole") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Adds a role to the system";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForCreateRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        return grid.catalogManager(txn)
            ->runUserManagementWriteCommand(txn, this->name, dbname, cmdObj, &result);
    }

} cmdCreateRole;

class CmdUpdateRole : public Command {
public:
    CmdUpdateRole() : Command("updateRole") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Used to update a role";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForUpdateRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdUpdateRole;

class CmdGrantPrivilegesToRole : public Command {
public:
    CmdGrantPrivilegesToRole() : Command("grantPrivilegesToRole") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Grants privileges to a role";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForGrantPrivilegesToRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdGrantPrivilegesToRole;

class CmdRevokePrivilegesFromRole : public Command {
public:
    CmdRevokePrivilegesFromRole() : Command("revokePrivilegesFromRole") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Revokes privileges from a role";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForRevokePrivilegesFromRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdRevokePrivilegesFromRole;

class CmdGrantRolesToRole : public Command {
public:
    CmdGrantRolesToRole() : Command("grantRolesToRole") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Grants roles to another role.";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForGrantRolesToRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdGrantRolesToRole;

class CmdRevokeRolesFromRole : public Command {
public:
    CmdRevokeRolesFromRole() : Command("revokeRolesFromRole") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Revokes roles from another role.";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForRevokeRolesFromRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdRevokeRolesFromRole;

class CmdDropRole : public Command {
public:
    CmdDropRole() : Command("dropRole") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Drops a single role.  Before deleting the role completely it must remove it "
              "from any users or roles that reference it.  If any errors occur in the middle "
              "of that process it's possible to be left in a state where the role has been "
              "removed from some user/roles but otherwise still exists.";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForDropRoleCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdDropRole;

class CmdDropAllRolesFromDatabase : public Command {
public:
    CmdDropAllRolesFromDatabase() : Command("dropAllRolesFromDatabase") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Drops all roles from the given database.  Before deleting the roles completely "
              "it must remove them from any users or other roles that reference them.  If any "
              "errors occur in the middle of that process it's possible to be left in a state "
              "where the roles have been removed from some user/roles but otherwise still "
              "exist.";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForDropAllRolesFromDatabaseCommand(client, dbname);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        const bool ok = grid.catalogManager(txn)->runUserManagementWriteCommand(
            txn, this->name, dbname, cmdObj, &result);

        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();

        return ok;
    }

} cmdDropAllRolesFromDatabase;

class CmdRolesInfo : public Command {
public:
    CmdRolesInfo() : Command("rolesInfo") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Returns information about roles.";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForRolesInfoCommand(client, dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        return grid.catalogManager(txn)->runUserManagementReadCommand(txn, dbname, cmdObj, &result);
    }

} cmdRolesInfo;

class CmdInvalidateUserCache : public Command {
public:
    CmdInvalidateUserCache() : Command("invalidateUserCache") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& ss) const {
        ss << "Invalidates the in-memory cache of user information";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForInvalidateUserCacheCommand(client);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        AuthorizationManager* authzManager = getGlobalAuthorizationManager();
        invariant(authzManager);
        authzManager->invalidateUserCache();
        return true;
    }

} cmdInvalidateUserCache;

/**
 * This command is used only by mongorestore to handle restoring users/roles.  We do this so
 * that mongorestore doesn't do direct inserts into the admin.system.users and
 * admin.system.roles, which would bypass the authzUpdateLock and allow multiple concurrent
 * modifications to users/roles.  What mongorestore now does instead is it inserts all user/role
 * definitions it wants to restore into temporary collections, then this command moves those
 * user/role definitions into their proper place in admin.system.users and admin.system.roles.
 * It either adds the users/roles to the existing ones or replaces the existing ones, depending
 * on whether the "drop" argument is true or false.
 */
class CmdMergeAuthzCollections : public Command {
public:
    CmdMergeAuthzCollections() : Command("_mergeAuthzCollections") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual void help(stringstream& ss) const {
        ss << "Internal command used by mongorestore for updating user/role data";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForMergeAuthzCollectionsCommand(client, cmdObj);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        return grid.catalogManager(txn)
            ->runUserManagementWriteCommand(txn, this->name, dbname, cmdObj, &result);
    }

} cmdMergeAuthzCollections;

namespace {
/**
 * Runs the authSchemaUpgrade on all shards, with the given maxSteps and writeConcern
 * parameters.
 *
 * Upgrades each shard serially, and stops on first failure.  Returned error indicates that
 * failure.
 */
Status runUpgradeOnAllShards(OperationContext* txn, int maxSteps, const BSONObj& writeConcern) {
    BSONObjBuilder cmdObjBuilder;
    cmdObjBuilder.append("authSchemaUpgrade", 1);
    cmdObjBuilder.append("maxSteps", maxSteps);
    if (!writeConcern.isEmpty()) {
        cmdObjBuilder.append("writeConcern", writeConcern);
    }
    const BSONObj cmdObj = cmdObjBuilder.done();

    // Upgrade each shard in turn, stopping on first failure.
    auto shardRegistry = grid.shardRegistry();
    shardRegistry->reload(txn);
    vector<string> shardIds;
    shardRegistry->getAllShardIds(&shardIds);

    for (const auto& shardId : shardIds) {
        auto cmdResult =
            shardRegistry->runCommandWithNotMasterRetries(txn, shardId, "admin", cmdObj);

        if (!cmdResult.isOK()) {
            return Status(cmdResult.getStatus().code(),
                          str::stream() << "Failed to run authSchemaUpgrade on shard " << shardId
                                        << causedBy(cmdResult.getStatus()));
        }
    }

    return Status::OK();
}
}  // namespace

class CmdAuthSchemaUpgrade : public Command {
public:
    CmdAuthSchemaUpgrade() : Command("authSchemaUpgrade") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return true;
    }

    virtual void help(stringstream& ss) const {
        ss << "Upgrades the auth data storage schema";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return auth::checkAuthForAuthSchemaUpgradeCommand(client);
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        // Run the authSchemaUpgrade command on the config servers
        if (!grid.catalogManager(txn)
                 ->runUserManagementWriteCommand(txn, this->name, dbname, cmdObj, &result)) {
            return false;
        }

        auth::AuthSchemaUpgradeArgs parsedArgs;
        Status status = auth::parseAuthSchemaUpgradeCommand(cmdObj, dbname, &parsedArgs);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }

        // Optionally run the authSchemaUpgrade command on the individual shards
        if (parsedArgs.shouldUpgradeShards) {
            status = runUpgradeOnAllShards(txn, parsedArgs.maxSteps, parsedArgs.writeConcern);
            if (!status.isOK())
                return appendCommandStatus(result, status);
        }
        return true;
    }
} cmdAuthSchemaUpgrade;

}  // namespace mongo
