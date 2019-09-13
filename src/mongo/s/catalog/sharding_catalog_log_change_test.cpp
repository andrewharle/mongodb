
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/future.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/text.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::TaskExecutor;
using stdx::async;
using unittest::assertGet;

const Seconds kFutureTimeout{5};
const HostAndPort configHost{"TestHost1"};

class InfoLoggingTest : public ShardingTestFixture {
public:
    enum CollType { ActionLog, ChangeLog };

    InfoLoggingTest(CollType configCollType, int cappedSize)
        : _configCollType(configCollType), _cappedSize(cappedSize) {}

    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(configHost);
    }

protected:
    void noRetryAfterSuccessfulCreate() {
        auto future = launchAsync([this] {
            log("moved a chunk", "foo.bar", BSON("min" << 3 << "max" << 4)).transitional_ignore();
        });

        expectConfigCollectionCreate(configHost, getConfigCollName(), _cappedSize, BSON("ok" << 1));
        expectConfigCollectionInsert(configHost,
                                     getConfigCollName(),
                                     network()->now(),
                                     "moved a chunk",
                                     "foo.bar",
                                     BSON("min" << 3 << "max" << 4));

        // Now wait for the logChange call to return
        future.timed_get(kFutureTimeout);

        // Now log another change and confirm that we don't re-attempt to create the collection
        future = launchAsync([this] {
            log("moved a second chunk", "foo.bar", BSON("min" << 4 << "max" << 5))
                .transitional_ignore();
        });

        expectConfigCollectionInsert(configHost,
                                     getConfigCollName(),
                                     network()->now(),
                                     "moved a second chunk",
                                     "foo.bar",
                                     BSON("min" << 4 << "max" << 5));

        // Now wait for the logChange call to return
        future.timed_get(kFutureTimeout);
    }

    void noRetryCreateIfAlreadyExists() {
        auto future = launchAsync([this] {
            log("moved a chunk", "foo.bar", BSON("min" << 3 << "max" << 4)).transitional_ignore();
        });

        BSONObjBuilder createResponseBuilder;
        CommandHelpers::appendCommandStatusNoThrow(
            createResponseBuilder, Status(ErrorCodes::NamespaceExists, "coll already exists"));
        expectConfigCollectionCreate(
            configHost, getConfigCollName(), _cappedSize, createResponseBuilder.obj());
        expectConfigCollectionInsert(configHost,
                                     getConfigCollName(),
                                     network()->now(),
                                     "moved a chunk",
                                     "foo.bar",
                                     BSON("min" << 3 << "max" << 4));

        // Now wait for the logAction call to return
        future.timed_get(kFutureTimeout);

        // Now log another change and confirm that we don't re-attempt to create the collection
        future = launchAsync([this] {
            log("moved a second chunk", "foo.bar", BSON("min" << 4 << "max" << 5))
                .transitional_ignore();
        });

        expectConfigCollectionInsert(configHost,
                                     getConfigCollName(),
                                     network()->now(),
                                     "moved a second chunk",
                                     "foo.bar",
                                     BSON("min" << 4 << "max" << 5));

        // Now wait for the logChange call to return
        future.timed_get(kFutureTimeout);
    }

    void createFailure() {
        auto future = launchAsync([this] {
            log("moved a chunk", "foo.bar", BSON("min" << 3 << "max" << 4)).transitional_ignore();
        });

        BSONObjBuilder createResponseBuilder;
        CommandHelpers::appendCommandStatusNoThrow(
            createResponseBuilder, Status(ErrorCodes::ExceededTimeLimit, "operation timed out"));
        expectConfigCollectionCreate(
            configHost, getConfigCollName(), _cappedSize, createResponseBuilder.obj());

        // Now wait for the logAction call to return
        future.timed_get(kFutureTimeout);

        // Now log another change and confirm that we *do* attempt to create the collection
        future = launchAsync([this] {
            log("moved a second chunk", "foo.bar", BSON("min" << 4 << "max" << 5))
                .transitional_ignore();
        });

        expectConfigCollectionCreate(configHost, getConfigCollName(), _cappedSize, BSON("ok" << 1));
        expectConfigCollectionInsert(configHost,
                                     getConfigCollName(),
                                     network()->now(),
                                     "moved a second chunk",
                                     "foo.bar",
                                     BSON("min" << 4 << "max" << 5));

        // Now wait for the logChange call to return
        future.timed_get(kFutureTimeout);
    }

    std::string getConfigCollName() const {
        return (_configCollType == ChangeLog ? "changelog" : "actionlog");
    }

    Status log(const std::string& what, const std::string& ns, const BSONObj& detail) {
        if (_configCollType == ChangeLog) {
            return catalogClient()->logChange(
                operationContext(), what, ns, detail, ShardingCatalogClient::kMajorityWriteConcern);
        } else {
            return catalogClient()->logAction(operationContext(), what, ns, detail);
        }
    }

    const CollType _configCollType;
    const int _cappedSize;
};

class ActionLogTest : public InfoLoggingTest {
public:
    ActionLogTest() : InfoLoggingTest(ActionLog, 20 * 1024 * 1024) {}
};

class ChangeLogTest : public InfoLoggingTest {
public:
    ChangeLogTest() : InfoLoggingTest(ChangeLog, 200 * 1024 * 1024) {}
};

TEST_F(ActionLogTest, NoRetryAfterSuccessfulCreate) {
    noRetryAfterSuccessfulCreate();
}
TEST_F(ChangeLogTest, NoRetryAfterSuccessfulCreate) {
    noRetryAfterSuccessfulCreate();
}

TEST_F(ActionLogTest, NoRetryCreateIfAlreadyExists) {
    noRetryCreateIfAlreadyExists();
}
TEST_F(ChangeLogTest, NoRetryCreateIfAlreadyExists) {
    noRetryCreateIfAlreadyExists();
}

TEST_F(ActionLogTest, CreateFailure) {
    createFailure();
}
TEST_F(ChangeLogTest, CreateFailure) {
    createFailure();
}

}  // namespace
}  // namespace mongo
