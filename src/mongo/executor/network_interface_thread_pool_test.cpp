
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/executor/async_timer_mock.h"
#include "mongo/executor/network_interface_asio.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool_test_common.h"
#include "mongo/util/concurrency/thread_pool_test_fixture.h"

namespace {
using namespace mongo;

class NetworkInterfaceThreadPoolWithASIO : public ThreadPoolInterface {
public:
    NetworkInterfaceThreadPoolWithASIO() {
        executor::NetworkInterfaceASIO::Options options;
        options.timerFactory = stdx::make_unique<executor::AsyncTimerFactoryMock>();
        _asio = stdx::make_unique<executor::NetworkInterfaceASIO>(std::move(options));
        _pool = stdx::make_unique<executor::NetworkInterfaceThreadPool>(_asio.get());
        _asio->startup();
    }

    ~NetworkInterfaceThreadPoolWithASIO() {
        _asio->shutdown();
    }

    void startup() override {
        _pool->startup();
    }

    void shutdown() override {
        _pool->shutdown();
    }

    void join() override {
        _pool->join();
    }

    Status schedule(Task task) override {
        return _pool->schedule(std::move(task));
    }

private:
    std::unique_ptr<executor::NetworkInterfaceASIO> _asio;
    std::unique_ptr<executor::NetworkInterfaceThreadPool> _pool;
};

MONGO_INITIALIZER(ThreadPoolCommonTests)(InitializerContext*) {
    addTestsForThreadPool("ThreadPoolCommon",
                          []() { return stdx::make_unique<NetworkInterfaceThreadPoolWithASIO>(); });
    return Status::OK();
}

}  // namespace
