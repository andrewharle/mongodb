
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

#include "mongo/db/auth/authz_session_external_state_d.h"

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"


namespace mongo {

AuthzSessionExternalStateMongod::AuthzSessionExternalStateMongod(AuthorizationManager* authzManager)
    : AuthzSessionExternalStateServerCommon(authzManager) {}
AuthzSessionExternalStateMongod::~AuthzSessionExternalStateMongod() {}

void AuthzSessionExternalStateMongod::startRequest(OperationContext* opCtx) {
    // No locks should be held as this happens before any database accesses occur
    dassert(!opCtx->lockState()->isLocked());

    _checkShouldAllowLocalhost(opCtx);
}

bool AuthzSessionExternalStateMongod::shouldIgnoreAuthChecks() const {
    // TODO(spencer): get "isInDirectClient" from OperationContext
    return cc().isInDirectClient() ||
        AuthzSessionExternalStateServerCommon::shouldIgnoreAuthChecks();
}

bool AuthzSessionExternalStateMongod::serverIsArbiter() const {
    // Arbiters have access to extra privileges under localhost. See SERVER-5479.
    return (
        repl::ReplicationCoordinator::get(getGlobalServiceContext())->getReplicationMode() ==
            repl::ReplicationCoordinator::modeReplSet &&
        repl::ReplicationCoordinator::get(getGlobalServiceContext())->getMemberState().arbiter());
}

MONGO_REGISTER_SHIM(AuthzSessionExternalState::create)
(AuthorizationManager* const authzManager)->std::unique_ptr<AuthzSessionExternalState> {
    return std::make_unique<AuthzSessionExternalStateMongod>(authzManager);
}

}  // namespace mongo
