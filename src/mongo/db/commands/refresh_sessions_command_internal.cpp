
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

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/refresh_sessions_gen.h"

namespace mongo {

class RefreshSessionsCommandInternal final : public BasicCommand {
    MONGO_DISALLOW_COPYING(RefreshSessionsCommandInternal);

public:
    RefreshSessionsCommandInternal() : BasicCommand("refreshSessionsInternal") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool adminOnly() const override {
        return false;
    }
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    std::string help() const override {
        return "renew a set of logical sessions";
    }
    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        // Must be authenticated as an internal cluster member.
        auto authSession = AuthorizationSession::get(opCtx->getClient());
        if (!authSession->isAuthorizedForPrivilege(
                Privilege(ResourcePattern::forClusterResource(), ActionType::impersonate))) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }
        return Status::OK();
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& db,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) override {
        IDLParserErrorContext ctx("RefreshSessionsCmdFromClusterMember");
        auto cmd = RefreshSessionsCmdFromClusterMember::parse(ctx, cmdObj);
        auto res =
            LogicalSessionCache::get(opCtx->getServiceContext())->refreshSessions(opCtx, cmd);
        uassertStatusOK(res);

        return true;
    }
} refreshSessionsCommandInternal;

}  // namespace mongo
