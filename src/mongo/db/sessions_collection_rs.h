
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

#pragma once

#include <memory>

#include "mongo/client/connection_string.h"
#include "mongo/client/connpool.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/util/time_support.h"

namespace mongo {

class DBDirectClient;
class OperationContext;
class RemoteCommandTargeter;

/**
 * Accesses the sessions collection for replica set members.
 */
class SessionsCollectionRS : public SessionsCollection {
public:
    /**
     * Constructs a new SessionsCollectionRS.
     */
    SessionsCollectionRS() = default;

    /**
     * Ensures that the sessions collection exists and has the proper indexes.
     */
    Status setupSessionsCollection(OperationContext* opCtx) override;

    /**
     * Checks if the sessions collection exists and has the proper indexes.
     */
    Status checkSessionsCollectionExists(OperationContext* opCtx) override;

    /**
     * Updates the last-use times on the given sessions to be greater than
     * or equal to the current time.
     *
     * If a step-down happens on this node as this method is running, it may fail.
     */
    Status refreshSessions(OperationContext* opCtx,
                           const LogicalSessionRecordSet& sessions) override;

    /**
     * Removes the authoritative records for the specified sessions.
     *
     * If a step-down happens on this node as this method is running, it may fail.
     */
    Status removeRecords(OperationContext* opCtx, const LogicalSessionIdSet& sessions) override;

    /**
     * Returns the subset of sessions from the given set that do not have entries
     * in the sessions collection.
     *
     * If a step-down happens on this node as this method is running, it may
     * return stale results.
     */
    StatusWith<LogicalSessionIdSet> findRemovedSessions(
        OperationContext* opCtx, const LogicalSessionIdSet& sessions) override;

    /**
     * Removes the transaction records for the specified sessions from the
     * transaction table.
     *
     * If a step-down happens on this node as this method is running, it may fail.
     */
    Status removeTransactionRecords(OperationContext* opCtx,
                                    const LogicalSessionIdSet& sessions) override;

    /**
     * Helper for a shard server to run its transaction operations as a replica set
     * member.
     *
     * If a step-down happens on this node as this method is running, it may fail.
     */
    static Status removeTransactionRecordsHelper(OperationContext* opCtx,
                                                 const LogicalSessionIdSet& sessions);
};

}  // namespace mongo
