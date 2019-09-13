
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

#include "mongo/client/authenticate.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/util/password_digest.h"

using mongo::executor::RemoteCommandRequest;

namespace mongo {
namespace auth {
namespace {

const StringData kUserSourceFieldName = "userSource"_sd;
const BSONObj kGetNonceCmd = BSON("getnonce" << 1);

StatusWith<std::string> extractDBField(const BSONObj& params) {
    std::string db;
    if (params.hasField(kUserSourceFieldName)) {
        if (!bsonExtractStringField(params, kUserSourceFieldName, &db).isOK()) {
            return {ErrorCodes::AuthenticationFailed, "userSource field must contain a string"};
        }
    } else {
        if (!bsonExtractStringField(params, saslCommandUserDBFieldName, &db).isOK()) {
            return {ErrorCodes::AuthenticationFailed, "db field must contain a string"};
        }
    }

    return std::move(db);
}

StatusWith<RemoteCommandRequest> createMongoCRGetNonceCmd(const BSONObj& params) {
    auto db = extractDBField(params);
    if (!db.isOK()) {
        return std::move(db.getStatus());
    }

    auto request = RemoteCommandRequest();
    request.cmdObj = kGetNonceCmd;
    request.dbname = db.getValue();

    return std::move(request);
}

RemoteCommandRequest createMongoCRAuthenticateCmd(const BSONObj& params, StringData nonce) {
    std::string username;
    uassertStatusOK(bsonExtractStringField(params, saslCommandUserFieldName, &username));

    std::string password;
    uassertStatusOK(bsonExtractStringField(params, saslCommandPasswordFieldName, &password));

    bool shouldDigest;
    uassertStatusOK(bsonExtractBooleanFieldWithDefault(
        params, saslCommandDigestPasswordFieldName, true, &shouldDigest));

    std::string digested = password;
    if (shouldDigest) {
        digested = createPasswordDigest(username, password);
    }

    auto request = RemoteCommandRequest();
    request.dbname = uassertStatusOK(extractDBField(params));

    BSONObjBuilder b;
    {
        b << "authenticate" << 1 << "nonce" << nonce << "user" << username;
        md5digest d;
        {
            md5_state_t st;
            md5_init(&st);
            md5_append(&st, reinterpret_cast<const md5_byte_t*>(nonce.rawData()), nonce.size());
            md5_append(&st, reinterpret_cast<const md5_byte_t*>(username.c_str()), username.size());
            md5_append(&st, reinterpret_cast<const md5_byte_t*>(digested.c_str()), digested.size());
            md5_finish(&st, d);
        }
        b << "key" << digestToString(d);
        request.cmdObj = b.obj();
    }
    return request;
}

void authMongoCRImpl(RunCommandHook runCommand,
                     const BSONObj& params,
                     AuthCompletionHandler handler) {
    invariant(runCommand);
    invariant(handler);

    // Step 1: send getnonce command, receive nonce
    auto nonceRequest = createMongoCRGetNonceCmd(params);
    if (!nonceRequest.isOK())
        return handler(std::move(nonceRequest.getStatus()));

    runCommand(nonceRequest.getValue(), [runCommand, params, handler](AuthResponse response) {
        if (!response.isOK())
            return handler(std::move(response));

        try {
            // Ensure response was valid
            std::string nonce;
            BSONObj nonceResponse = response.data;
            auto valid = bsonExtractStringField(nonceResponse, "nonce", &nonce);
            if (!valid.isOK())
                return handler({ErrorCodes::AuthenticationFailed,
                                "Invalid nonce response: " + nonceResponse.toString()});

            // Step 2: send authenticate command, receive response
            runCommand(createMongoCRAuthenticateCmd(params, nonce), handler);
        } catch (const DBException& e) {
            return handler(e.toStatus());
        }
    });
}

MONGO_INITIALIZER(RegisterAuthMongoCR)(InitializerContext* context) {
    authMongoCR = authMongoCRImpl;
    return Status::OK();
}

}  // namespace
}  // namespace auth
}  // namespace mongo
