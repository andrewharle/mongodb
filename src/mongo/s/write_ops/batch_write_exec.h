
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

#include <map>
#include <string>

#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/s/ns_targeter.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {

class BatchWriteExecStats;
class OperationContext;

/**
 * The BatchWriteExec is able to execute client batch write requests, resulting in a batch
 * response to send back to the client.
 *
 * There are two main interfaces the exec uses to "run" the batch:
 *
 *  - the "targeter" used to generate child batch operations to send to particular shards
 *
 *  - the "dispatcher" used to send child batches to several shards at once, and retrieve the
 *    results
 *
 * Both the targeter and dispatcher are assumed to be dedicated to this particular
 * BatchWriteExec instance.
 *
 */
class BatchWriteExec {
public:
    /**
     * Executes a client batch write request by sending child batches to several shard
     * endpoints, and returns a client batch write response.
     *
     * This function does not throw, any errors are reported via the clientResponse.
     */
    static void executeBatch(OperationContext* opCtx,
                             NSTargeter& targeter,
                             const BatchedCommandRequest& clientRequest,
                             BatchedCommandResponse* clientResponse,
                             BatchWriteExecStats* stats);
};

struct HostOpTime {
    HostOpTime(repl::OpTime ot, OID e) : opTime(ot), electionId(e){};
    HostOpTime(){};
    repl::OpTime opTime;
    OID electionId;
};

typedef std::map<ConnectionString, HostOpTime> HostOpTimeMap;

class BatchWriteExecStats {
public:
    BatchWriteExecStats()
        : numRounds(0), numTargetErrors(0), numResolveErrors(0), numStaleBatches(0) {}

    void noteWriteAt(const HostAndPort& host, repl::OpTime opTime, const OID& electionId);
    void noteTargetedShard(const ShardId& shardId);

    const std::set<ShardId>& getTargetedShards() const;
    const HostOpTimeMap& getWriteOpTimes() const;

    // Expose via helpers if this gets more complex

    // Number of round trips required for the batch
    int numRounds;
    // Number of times targeting failed
    int numTargetErrors;
    // Number of times host resolution failed
    int numResolveErrors;
    // Number of stale batches
    int numStaleBatches;

private:
    std::set<ShardId> _targetedShards;
    HostOpTimeMap _writeOpTimes;
};

}  // namespace mongo
