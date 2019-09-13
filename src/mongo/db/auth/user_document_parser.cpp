
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/db/auth/user_document_parser.h"

#include <string>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {

namespace {
const std::string ADMIN_DBNAME = "admin";

const std::string ROLES_FIELD_NAME = "roles";
const std::string PRIVILEGES_FIELD_NAME = "inheritedPrivileges";
const std::string INHERITED_ROLES_FIELD_NAME = "inheritedRoles";
const std::string OTHER_DB_ROLES_FIELD_NAME = "otherDBRoles";
const std::string READONLY_FIELD_NAME = "readOnly";
const std::string CREDENTIALS_FIELD_NAME = "credentials";
const std::string ROLE_NAME_FIELD_NAME = "role";
const std::string ROLE_DB_FIELD_NAME = "db";
const std::string SCRAMSHA1_CREDENTIAL_FIELD_NAME = "SCRAM-SHA-1";
const std::string SCRAMSHA256_CREDENTIAL_FIELD_NAME = "SCRAM-SHA-256";
const std::string MONGODB_EXTERNAL_CREDENTIAL_FIELD_NAME = "external";
constexpr StringData AUTHENTICATION_RESTRICTIONS_FIELD_NAME = "authenticationRestrictions"_sd;
constexpr StringData INHERITED_AUTHENTICATION_RESTRICTIONS_FIELD_NAME =
    "inheritedAuthenticationRestrictions"_sd;

inline Status _badValue(const char* reason) {
    return Status(ErrorCodes::BadValue, reason);
}

inline Status _badValue(const std::string& reason) {
    return Status(ErrorCodes::BadValue, reason);
}

template <typename Credentials>
bool parseSCRAMCredentials(const BSONElement& credentialsElement,
                           Credentials& scram,
                           const std::string& fieldName) {
    const auto scramElement = credentialsElement[fieldName];
    if (scramElement.eoo()) {
        return false;
    }

    // We are asserting rather then returning errors since these
    // fields should have been prepopulated by the calling code.
    scram.iterationCount = scramElement["iterationCount"].numberInt();
    uassert(17501,
            str::stream() << "Invalid or missing " << fieldName << " iteration count",
            scram.iterationCount > 0);

    scram.salt = scramElement["salt"].str();
    uassert(17502, str::stream() << "Missing " << fieldName << " salt", !scram.salt.empty());

    scram.serverKey = scramElement["serverKey"].str();
    uassert(
        17503, str::stream() << "Missing " << fieldName << " serverKey", !scram.serverKey.empty());

    scram.storedKey = scramElement["storedKey"].str();
    uassert(
        17504, str::stream() << "Missing " << fieldName << " storedKey", !scram.storedKey.empty());

    uassert(50684,
            str::stream() << "credential document " << fieldName << " failed validation",
            scram.isValid());
    return true;
}

}  // namespace

Status _checkV2RolesArray(const BSONElement& rolesElement) {
    if (rolesElement.eoo()) {
        return _badValue("User document needs 'roles' field to be provided");
    }
    if (rolesElement.type() != Array) {
        return _badValue("'roles' field must be an array");
    }
    for (BSONObjIterator iter(rolesElement.embeddedObject()); iter.more(); iter.next()) {
        if ((*iter).type() != Object) {
            return _badValue("Elements in 'roles' array must objects");
        }
        Status status = V2UserDocumentParser::checkValidRoleObject((*iter).Obj());
        if (!status.isOK())
            return status;
    }
    return Status::OK();
}

Status V2UserDocumentParser::checkValidUserDocument(const BSONObj& doc) const {
    auto userIdElement = doc[AuthorizationManager::USERID_FIELD_NAME];
    auto userElement = doc[AuthorizationManager::USER_NAME_FIELD_NAME];
    auto userDBElement = doc[AuthorizationManager::USER_DB_FIELD_NAME];
    auto credentialsElement = doc[CREDENTIALS_FIELD_NAME];
    auto rolesElement = doc[ROLES_FIELD_NAME];

    // Validate the "userId" element.
    if (!userIdElement.eoo()) {
        if (!userIdElement.isBinData(BinDataType::newUUID)) {
            return _badValue("User document needs 'userId' field to be a UUID");
        }
    }

    // Validate the "user" element.
    if (userElement.type() != String)
        return _badValue("User document needs 'user' field to be a string");
    if (userElement.valueStringData().empty())
        return _badValue("User document needs 'user' field to be non-empty");

    // Validate the "db" element
    if (userDBElement.type() != String || userDBElement.valueStringData().empty()) {
        return _badValue("User document needs 'db' field to be a non-empty string");
    }
    StringData userDBStr = userDBElement.valueStringData();
    if (!NamespaceString::validDBName(userDBStr, NamespaceString::DollarInDbNameBehavior::Allow) &&
        userDBStr != "$external") {
        return _badValue(mongoutils::str::stream() << "'" << userDBStr
                                                   << "' is not a valid value for the db field.");
    }

    // Validate the "credentials" element
    if (credentialsElement.eoo()) {
        return _badValue("User document needs 'credentials' object");
    }
    if (credentialsElement.type() != Object) {
        return _badValue("User document needs 'credentials' field to be an object");
    }

    BSONObj credentialsObj = credentialsElement.Obj();
    if (credentialsObj.isEmpty()) {
        return _badValue("User document needs 'credentials' field to be a non-empty object");
    }
    if (userDBStr == "$external") {
        BSONElement externalElement = credentialsObj[MONGODB_EXTERNAL_CREDENTIAL_FIELD_NAME];
        if (externalElement.eoo() || externalElement.type() != Bool || !externalElement.Bool()) {
            return _badValue(
                "User documents for users defined on '$external' must have "
                "'credentials' field set to {external: true}");
        }
    } else {
        const auto validateScram = [&credentialsObj](const auto& fieldName) {
            auto scramElement = credentialsObj[fieldName];

            if (scramElement.eoo()) {
                return Status(ErrorCodes::NoSuchKey,
                              str::stream() << fieldName << " does not exist");
            }
            if (scramElement.type() != Object) {
                return _badValue(str::stream() << fieldName
                                               << " credential must be an object, if present");
            }
            return Status::OK();
        };

        const auto sha1status = validateScram(SCRAMSHA1_CREDENTIAL_FIELD_NAME);
        if (!sha1status.isOK() && (sha1status.code() != ErrorCodes::NoSuchKey)) {
            return sha1status;
        }
        const auto sha256status = validateScram(SCRAMSHA256_CREDENTIAL_FIELD_NAME);
        if (!sha256status.isOK() && (sha256status.code() != ErrorCodes::NoSuchKey)) {
            return sha256status;
        }

        if (!sha1status.isOK() && !sha256status.isOK()) {
            return _badValue(
                "User document must provide credentials for all "
                "non-external users");
        }
    }

    // Validate the "roles" element.
    Status status = _checkV2RolesArray(rolesElement);
    if (!status.isOK())
        return status;

    // Validate the "authenticationRestrictions" element.
    status = initializeAuthenticationRestrictionsFromUserDocument(doc, nullptr);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

User::UserId V2UserDocumentParser::extractUserIDFromUserDocument(const BSONObj& doc) const {
    auto userId = doc[AuthorizationManager::USERID_FIELD_NAME];
    if (userId.isBinData(BinDataType::newUUID)) {
        auto id = userId.uuid();
        User::UserId ret;
        std::copy(id.begin(), id.end(), std::back_inserter(ret));
        return ret;
    }

    return User::UserId();
}

std::string V2UserDocumentParser::extractUserNameFromUserDocument(const BSONObj& doc) const {
    return doc[AuthorizationManager::USER_NAME_FIELD_NAME].str();
}

Status V2UserDocumentParser::initializeUserCredentialsFromUserDocument(
    User* user, const BSONObj& privDoc) const {
    User::CredentialData credentials;
    std::string userDB = privDoc[AuthorizationManager::USER_DB_FIELD_NAME].String();
    BSONElement credentialsElement = privDoc[CREDENTIALS_FIELD_NAME];
    if (!credentialsElement.eoo()) {
        if (credentialsElement.type() != Object) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "'credentials' field in user documents must be an object");
        }
        if (userDB == "$external") {
            BSONElement externalCredentialElement =
                credentialsElement.Obj()[MONGODB_EXTERNAL_CREDENTIAL_FIELD_NAME];
            if (!externalCredentialElement.eoo()) {
                if (externalCredentialElement.type() != Bool || !externalCredentialElement.Bool()) {
                    return Status(ErrorCodes::UnsupportedFormat,
                                  "'external' field in credentials object must be set to true");
                } else {
                    credentials.isExternal = true;
                }
            } else {
                return Status(ErrorCodes::UnsupportedFormat,
                              "User documents defined on '$external' must provide set "
                              "credentials to {external:true}");
            }
        } else {
            const bool haveSha1 = parseSCRAMCredentials(
                credentialsElement, credentials.scram_sha1, SCRAMSHA1_CREDENTIAL_FIELD_NAME);
            const bool haveSha256 = parseSCRAMCredentials(
                credentialsElement, credentials.scram_sha256, SCRAMSHA256_CREDENTIAL_FIELD_NAME);

            if (!haveSha1 && !haveSha256) {
                return Status(
                    ErrorCodes::UnsupportedFormat,
                    "User documents must provide credentials for SCRAM-SHA-1 and/or SCRAM-SHA-256");
            }

            credentials.isExternal = false;
        }
    } else {
        return Status(ErrorCodes::UnsupportedFormat,
                      "Cannot extract credentials from user documents without a "
                      "'credentials' field");
    }

    user->setCredentials(credentials);
    return Status::OK();
}

static Status _extractRoleDocumentElements(const BSONObj& roleObject,
                                           BSONElement* roleNameElement,
                                           BSONElement* roleSourceElement) {
    *roleNameElement = roleObject[ROLE_NAME_FIELD_NAME];
    *roleSourceElement = roleObject[ROLE_DB_FIELD_NAME];

    if (roleNameElement->type() != String || roleNameElement->valueStringData().empty()) {
        return Status(ErrorCodes::UnsupportedFormat, "Role names must be non-empty strings");
    }
    if (roleSourceElement->type() != String || roleSourceElement->valueStringData().empty()) {
        return Status(ErrorCodes::UnsupportedFormat, "Role db must be non-empty strings");
    }

    return Status::OK();
}

Status V2UserDocumentParser::checkValidRoleObject(const BSONObj& roleObject) {
    BSONElement roleNameElement;
    BSONElement roleSourceElement;
    return _extractRoleDocumentElements(roleObject, &roleNameElement, &roleSourceElement);
}

Status V2UserDocumentParser::parseRoleName(const BSONObj& roleObject, RoleName* result) {
    BSONElement roleNameElement;
    BSONElement roleSourceElement;
    Status status = _extractRoleDocumentElements(roleObject, &roleNameElement, &roleSourceElement);
    if (!status.isOK())
        return status;
    *result = RoleName(roleNameElement.str(), roleSourceElement.str());
    return status;
}

Status V2UserDocumentParser::parseRoleVector(const BSONArray& rolesArray,
                                             std::vector<RoleName>* result) {
    std::vector<RoleName> roles;
    for (BSONObjIterator it(rolesArray); it.more(); it.next()) {
        if ((*it).type() != Object) {
            return Status(ErrorCodes::TypeMismatch, "Roles must be objects.");
        }
        RoleName role;
        Status status = parseRoleName((*it).Obj(), &role);
        if (!status.isOK())
            return status;
        roles.push_back(role);
    }
    std::swap(*result, roles);
    return Status::OK();
}

Status V2UserDocumentParser::initializeAuthenticationRestrictionsFromUserDocument(
    const BSONObj& privDoc, User* user) const {
    RestrictionDocuments::sequence_type restrictionVector;

    // Restrictions on the user
    const auto authenticationRestrictions = privDoc[AUTHENTICATION_RESTRICTIONS_FIELD_NAME];
    if (!authenticationRestrictions.eoo()) {
        if (authenticationRestrictions.type() != Array) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "'authenticationRestrictions' field must be an array");
        }

        auto restrictions =
            parseAuthenticationRestriction(BSONArray(authenticationRestrictions.Obj()));
        if (!restrictions.isOK()) {
            return restrictions.getStatus();
        }

        restrictionVector.push_back(restrictions.getValue());
    }

    // Restrictions from roles
    const auto inherited = privDoc[INHERITED_AUTHENTICATION_RESTRICTIONS_FIELD_NAME];
    if (!inherited.eoo()) {
        if (inherited.type() != Array) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "'inheritedAuthenticationRestrictions' field must be an array");
        }

        for (const auto& roleRestriction : BSONArray(inherited.Obj())) {
            if (roleRestriction.type() != Array) {
                return Status(ErrorCodes::UnsupportedFormat,
                              "'inheritedAuthenticationRestrictions' sub-fields must be arrays");
            }

            auto roleRestrictionDoc =
                parseAuthenticationRestriction(BSONArray(roleRestriction.Obj()));
            if (!roleRestrictionDoc.isOK()) {
                return roleRestrictionDoc.getStatus();
            }

            restrictionVector.push_back(roleRestrictionDoc.getValue());
        }
    }

    if (user) {
        user->setRestrictions(RestrictionDocuments(restrictionVector));
    }

    return Status::OK();
}

Status V2UserDocumentParser::initializeUserRolesFromUserDocument(const BSONObj& privDoc,
                                                                 User* user) const {
    BSONElement rolesElement = privDoc[ROLES_FIELD_NAME];

    if (rolesElement.type() != Array) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "User document needs 'roles' field to be an array");
    }

    std::vector<RoleName> roles;
    for (BSONObjIterator it(rolesElement.Obj()); it.more(); it.next()) {
        if ((*it).type() != Object) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "User document needs values in 'roles' array to be a sub-documents");
        }
        BSONObj roleObject = (*it).Obj();

        RoleName role;
        Status status = parseRoleName(roleObject, &role);
        if (!status.isOK()) {
            return status;
        }
        roles.push_back(role);
    }
    user->setRoles(makeRoleNameIteratorForContainer(roles));
    return Status::OK();
}

Status V2UserDocumentParser::initializeUserIndirectRolesFromUserDocument(const BSONObj& privDoc,
                                                                         User* user) const {
    BSONElement indirectRolesElement = privDoc[INHERITED_ROLES_FIELD_NAME];

    if (indirectRolesElement.type() != Array) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "User document needs 'inheritedRoles' field to be an array");
    }

    std::vector<RoleName> indirectRoles;
    for (BSONObjIterator it(indirectRolesElement.Obj()); it.more(); it.next()) {
        if ((*it).type() != Object) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "User document needs values in 'inheritedRoles'"
                          " array to be a sub-documents");
        }
        BSONObj indirectRoleObject = (*it).Obj();

        RoleName indirectRole;
        Status status = parseRoleName(indirectRoleObject, &indirectRole);
        if (!status.isOK()) {
            return status;
        }
        indirectRoles.push_back(indirectRole);
    }
    user->setIndirectRoles(makeRoleNameIteratorForContainer(indirectRoles));
    return Status::OK();
}

Status V2UserDocumentParser::initializeUserPrivilegesFromUserDocument(const BSONObj& doc,
                                                                      User* user) const {
    BSONElement privilegesElement = doc[PRIVILEGES_FIELD_NAME];
    if (privilegesElement.eoo())
        return Status::OK();
    if (privilegesElement.type() != Array) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "User document 'inheritedPrivileges' element must be Array if present.");
    }
    PrivilegeVector privileges;
    std::string errmsg;
    for (BSONObjIterator it(privilegesElement.Obj()); it.more(); it.next()) {
        if ((*it).type() != Object) {
            warning() << "Wrong type of element in inheritedPrivileges array for "
                      << user->getName() << ": " << *it;
            continue;
        }
        Privilege privilege;
        ParsedPrivilege pp;
        if (!pp.parseBSON((*it).Obj(), &errmsg)) {
            warning() << "Could not parse privilege element in user document for "
                      << user->getName() << ": " << errmsg;
            continue;
        }
        std::vector<std::string> unrecognizedActions;
        Status status =
            ParsedPrivilege::parsedPrivilegeToPrivilege(pp, &privilege, &unrecognizedActions);
        if (!status.isOK()) {
            warning() << "Could not parse privilege element in user document for "
                      << user->getName() << causedBy(status);
            continue;
        }
        if (unrecognizedActions.size()) {
            std::string unrecognizedActionsString;
            joinStringDelim(unrecognizedActions, &unrecognizedActionsString, ',');
            warning() << "Encountered unrecognized actions \" " << unrecognizedActionsString
                      << "\" while parsing user document for " << user->getName();
        }
        privileges.push_back(privilege);
    }
    user->setPrivileges(privileges);
    return Status::OK();
}

}  // namespace mongo
