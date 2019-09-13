
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

#include "mongo/client/global_conn_pool.h"

#include "mongo/base/init.h"
#include "mongo/db/server_parameters.h"

namespace mongo {
namespace {

// Maximum connections per host the connection pool should store
int maxConnsPerHost(200);
ExportedServerParameter<int, ServerParameterType::kStartupOnly>  //
    maxConnsPerHostParameter(ServerParameterSet::getGlobal(),
                             "connPoolMaxConnsPerHost",
                             &maxConnsPerHost);

// Maximum in-use connections per host in the global connection pool
int maxInUseConnsPerHost(std::numeric_limits<int>::max());
ExportedServerParameter<int, ServerParameterType::kStartupOnly>  //
    maxInUseConnsPerHostParameter(ServerParameterSet::getGlobal(),
                                  "connPoolMaxInUseConnsPerHost",
                                  &maxInUseConnsPerHost);

// Amount of time, in minutes, to keep idle connections in the global connection pool
int globalConnPoolIdleTimeout(std::numeric_limits<int>::max());
ExportedServerParameter<int, ServerParameterType::kStartupOnly>  //
    globalConnPoolIdleTimeoutParameter(ServerParameterSet::getGlobal(),
                                       "globalConnPoolIdleTimeoutMinutes",
                                       &globalConnPoolIdleTimeout);

MONGO_INITIALIZER(InitializeGlobalConnectionPool)(InitializerContext* context) {
    globalConnPool.setName("connection pool");
    globalConnPool.setMaxPoolSize(maxConnsPerHost);
    globalConnPool.setMaxInUse(maxInUseConnsPerHost);
    globalConnPool.setIdleTimeout(globalConnPoolIdleTimeout);

    return Status::OK();
}

}  // namespace

DBConnectionPool globalConnPool;

ReplicaSetMonitorManager globalRSMonitorManager;

}  // namespace mongo
