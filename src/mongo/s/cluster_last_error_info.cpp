
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

#include "mongo/s/cluster_last_error_info.h"

#include "mongo/client/connection_string.h"
#include "mongo/db/lasterror.h"

namespace mongo {

const Client::Decoration<std::shared_ptr<ClusterLastErrorInfo>> ClusterLastErrorInfo::get =
    Client::declareDecoration<std::shared_ptr<ClusterLastErrorInfo>>();

void ClusterLastErrorInfo::addShardHost(const std::string& shardHost) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _cur->shardHostsWritten.insert(shardHost);
}

void ClusterLastErrorInfo::addHostOpTime(ConnectionString connStr, HostOpTime stat) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _cur->hostOpTimes[connStr] = stat;
}

void ClusterLastErrorInfo::addHostOpTimes(const HostOpTimeMap& hostOpTimes) {
    for (HostOpTimeMap::const_iterator it = hostOpTimes.begin(); it != hostOpTimes.end(); ++it) {
        addHostOpTime(it->first, it->second);
    }
}

void ClusterLastErrorInfo::newRequest() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    std::swap(_cur, _prev);
    _cur->clear();
}

void ClusterLastErrorInfo::disableForCommand() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    RequestInfo* temp = _cur;
    _cur = _prev;
    _prev = temp;
}

}  // namespace mongo
