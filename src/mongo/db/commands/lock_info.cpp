
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

#include <map>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

using std::string;
using std::stringstream;

/**
 * Admin command to display global lock information
 */
class CmdLockInfo : public BasicCommand {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const {
        return false;
    }

    std::string help() const override {
        return "show all lock info on the server";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const final {
        bool isAuthorized = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(), ActionType::serverStatus);
        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    CmdLockInfo() : BasicCommand("lockInfo") {}

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        auto lockToClientMap = LockManager::getLockToClientMap(opCtx->getServiceContext());
        getGlobalLockManager()->getLockInfoBSON(lockToClientMap, &result);
        return true;
    }
} cmdLockInfo;
}
