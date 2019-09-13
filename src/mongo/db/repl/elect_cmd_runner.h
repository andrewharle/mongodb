
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

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/oid.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"
#include "mongo/executor/task_executor.h"

namespace mongo {

class Status;

namespace repl {

class ReplSetConfig;
class ScatterGatherRunner;

class ElectCmdRunner {
    MONGO_DISALLOW_COPYING(ElectCmdRunner);

public:
    class Algorithm : public ScatterGatherAlgorithm {
    public:
        Algorithm(const ReplSetConfig& rsConfig,
                  int selfIndex,
                  const std::vector<HostAndPort>& targets,
                  OID round);

        virtual ~Algorithm();
        virtual std::vector<executor::RemoteCommandRequest> getRequests() const;
        virtual void processResponse(const executor::RemoteCommandRequest& request,
                                     const executor::RemoteCommandResponse& response);
        virtual bool hasReceivedSufficientResponses() const;

        int getReceivedVotes() const {
            return _receivedVotes;
        }

    private:
        // Tally of the number of received votes for this election.
        int _receivedVotes;

        // Number of responses received so far.
        size_t _actualResponses;

        bool _sufficientResponsesReceived;

        const ReplSetConfig _rsConfig;
        const int _selfIndex;
        const std::vector<HostAndPort> _targets;
        const OID _round;
    };

    ElectCmdRunner();
    ~ElectCmdRunner();

    /**
     * Begins the process of sending replSetElect commands to all non-DOWN nodes
     * in currentConfig.
     *
     * Returned handle can be used to schedule a callback when the process is complete.
     */
    StatusWith<executor::TaskExecutor::EventHandle> start(executor::TaskExecutor* executor,
                                                          const ReplSetConfig& currentConfig,
                                                          int selfIndex,
                                                          const std::vector<HostAndPort>& targets);

    /**
     * Informs the ElectCmdRunner to cancel further processing.
     */
    void cancel();

    /**
     * Returns the number of received votes.  Only valid to call after
     * the event handle returned from start() has been signaled, which guarantees that
     * the vote count will no longer be touched by callbacks.
     */
    int getReceivedVotes() const;

    /**
     * Returns true if cancel() was called on this instance.
     */
    bool isCanceled() const {
        return _isCanceled;
    }

private:
    std::shared_ptr<Algorithm> _algorithm;
    std::unique_ptr<ScatterGatherRunner> _runner;
    bool _isCanceled;
};
}
}
