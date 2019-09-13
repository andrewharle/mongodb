
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

#include <set>
#include <string>

#include "mongo/db/client.h"
#include "mongo/s/write_ops/batch_write_exec.h"

namespace mongo {

/**
 * Client decoration that holds information needed to by mongos to process
 * getLastError commands.
 */
class ClusterLastErrorInfo {
public:
    static const Client::Decoration<std::shared_ptr<ClusterLastErrorInfo>> get;

    /** new request not associated (yet or ever) with a client */
    void newRequest();

    /**
     * notes that this client use this shard
     * keeps track of all shards accessed this request
     */
    void addShardHost(const std::string& shardHost);

    /**
     * Notes that this client wrote to these particular hosts with write commands.
     */
    void addHostOpTime(ConnectionString connstr, HostOpTime stat);
    void addHostOpTimes(const HostOpTimeMap& hostOpTimes);

    /**
     * gets shards used on the previous request
     */
    std::set<std::string>* getPrevShardHosts() const {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return &_prev->shardHostsWritten;
    }

    /**
     * Gets the shards, hosts, and opTimes the client last wrote to with write commands.
     */
    const HostOpTimeMap& getPrevHostOpTimes() const {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _prev->hostOpTimes;
    }

    void disableForCommand();

private:
    struct RequestInfo {
        void clear() {
            shardHostsWritten.clear();
            hostOpTimes.clear();
        }

        std::set<std::string> shardHostsWritten;
        HostOpTimeMap hostOpTimes;
    };

    // Protects _infos, _cur, and _prev.
    mutable stdx::mutex _mutex;

    // We use 2 so we can flip for getLastError type operations.
    RequestInfo _infos[2];
    RequestInfo* _cur = &_infos[0];
    RequestInfo* _prev = &_infos[1];
};

}  // namespace mongo
