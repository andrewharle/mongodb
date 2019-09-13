
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

#include "mongo/dbtests/mock/mock_conn_registry.h"

#include "mongo/base/init.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"

namespace mongo {

using std::string;

std::unique_ptr<MockConnRegistry> MockConnRegistry::_instance;

MONGO_INITIALIZER(MockConnRegistry)(InitializerContext* context) {
    return MockConnRegistry::init();
}

Status MockConnRegistry::init() {
    MockConnRegistry::_instance.reset(new MockConnRegistry());
    return Status::OK();
}

MockConnRegistry::MockConnRegistry() : _mockConnStrHook(this) {}

MockConnRegistry* MockConnRegistry::get() {
    return _instance.get();
}

ConnectionString::ConnectionHook* MockConnRegistry::getConnStrHook() {
    return &_mockConnStrHook;
}

void MockConnRegistry::addServer(MockRemoteDBServer* server) {
    stdx::lock_guard<stdx::mutex> sl(_registryMutex);

    const std::string hostName(server->getServerAddress());
    fassert(16533, _registry.count(hostName) == 0);

    _registry[hostName] = server;
}

bool MockConnRegistry::removeServer(const std::string& hostName) {
    stdx::lock_guard<stdx::mutex> sl(_registryMutex);
    return _registry.erase(hostName) == 1;
}

void MockConnRegistry::clear() {
    stdx::lock_guard<stdx::mutex> sl(_registryMutex);
    _registry.clear();
}

std::unique_ptr<MockDBClientConnection> MockConnRegistry::connect(const std::string& connStr) {
    stdx::lock_guard<stdx::mutex> sl(_registryMutex);
    fassert(16534, _registry.count(connStr) == 1);
    return stdx::make_unique<MockDBClientConnection>(_registry[connStr], true);
}

MockConnRegistry::MockConnHook::MockConnHook(MockConnRegistry* registry) : _registry(registry) {}

MockConnRegistry::MockConnHook::~MockConnHook() {}

std::unique_ptr<mongo::DBClientBase> MockConnRegistry::MockConnHook::connect(
    const ConnectionString& connString, std::string& errmsg, double socketTimeout) {
    const string hostName(connString.toString());
    auto conn = _registry->connect(hostName);

    if (!conn->connect(hostName.c_str(), StringData(), errmsg)) {
        // mimic ConnectionString::connect for MASTER type connection to return NULL
        // if the destination is unreachable.
        return nullptr;
    }

    return std::move(conn);
}
}  // namespace mongo
