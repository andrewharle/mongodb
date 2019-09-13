
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

#include "mongo/db/commands/killcursors_common.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/killcursors_request.h"
#include "mongo/db/query/killcursors_response.h"

namespace mongo {

Status KillCursorsCmdBase::checkAuthForCommand(Client* client,
                                               const std::string& dbname,
                                               const BSONObj& cmdObj) const {
    const auto statusWithRequest = KillCursorsRequest::parseFromBSON(dbname, cmdObj);
    if (!statusWithRequest.isOK()) {
        return statusWithRequest.getStatus();
    }
    const auto killCursorsRequest = statusWithRequest.getValue();

    const auto& nss = killCursorsRequest.nss;
    for (CursorId id : killCursorsRequest.cursorIds) {
        const auto status = _checkAuth(client, nss, id);
        if (!status.isOK()) {
            if (status.code() == ErrorCodes::CursorNotFound) {
                // Not found isn't an authorization issue.
                // run() will raise it as a return value.
                continue;
            }
            audit::logKillCursorsAuthzCheck(client, nss, id, status.code());
            return status;
        }
    }

    return Status::OK();
}

bool KillCursorsCmdBase::run(OperationContext* opCtx,
                             const std::string& dbname,
                             const BSONObj& cmdObj,
                             BSONObjBuilder& result) {
    auto statusWithRequest = KillCursorsRequest::parseFromBSON(dbname, cmdObj);
    uassertStatusOK(statusWithRequest.getStatus());
    auto killCursorsRequest = std::move(statusWithRequest.getValue());

    std::vector<CursorId> cursorsKilled;
    std::vector<CursorId> cursorsNotFound;
    std::vector<CursorId> cursorsAlive;
    std::vector<CursorId> cursorsUnknown;

    for (CursorId id : killCursorsRequest.cursorIds) {
        Status status = _killCursor(opCtx, killCursorsRequest.nss, id);
        if (status.isOK()) {
            cursorsKilled.push_back(id);
        } else if (status.code() == ErrorCodes::CursorNotFound) {
            cursorsNotFound.push_back(id);
        } else {
            cursorsAlive.push_back(id);
        }

        audit::logKillCursorsAuthzCheck(
            opCtx->getClient(), killCursorsRequest.nss, id, status.code());
    }

    KillCursorsResponse killCursorsResponse(
        cursorsKilled, cursorsNotFound, cursorsAlive, cursorsUnknown);
    killCursorsResponse.addToBSON(&result);
    return true;
}

}  // namespace mongo
