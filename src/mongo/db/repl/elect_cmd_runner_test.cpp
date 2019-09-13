
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

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"


using std::unique_ptr;

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

class ElectCmdRunnerTest : public executor::ThreadPoolExecutorTest {
public:
    void startTest(ElectCmdRunner* electCmdRunner,
                   const ReplSetConfig& currentConfig,
                   int selfIndex,
                   const std::vector<HostAndPort>& hosts);

    void waitForTest();

    void electCmdRunnerRunner(const executor::TaskExecutor::CallbackArgs& data,
                              ElectCmdRunner* electCmdRunner,
                              StatusWith<executor::TaskExecutor::EventHandle>* evh,
                              const ReplSetConfig& currentConfig,
                              int selfIndex,
                              const std::vector<HostAndPort>& hosts);

private:
    void setUp() {
        executor::ThreadPoolExecutorTest::setUp();
        launchExecutorThread();
    }
    executor::TaskExecutor::EventHandle _allDoneEvent;
};

ReplSetConfig assertMakeRSConfig(const BSONObj& configBson) {
    ReplSetConfig config;
    ASSERT_OK(config.initialize(configBson));
    ASSERT_OK(config.validate());
    return config;
}

const BSONObj makeElectRequest(const ReplSetConfig& rsConfig, int selfIndex) {
    const MemberConfig& myConfig = rsConfig.getMemberAt(selfIndex);
    return BSON("replSetElect" << 1 << "set" << rsConfig.getReplSetName() << "who"
                               << myConfig.getHostAndPort().toString()
                               << "whoid"
                               << myConfig.getId()
                               << "cfgver"
                               << rsConfig.getConfigVersion()
                               << "round"
                               << 380865962699346850ll);
}

BSONObj stripRound(const BSONObj& orig) {
    BSONObjBuilder builder;
    for (BSONObjIterator iter(orig); iter.more(); iter.next()) {
        BSONElement e = *iter;
        if (e.fieldNameStringData() == "round") {
            continue;
        }
        builder.append(e);
    }
    return builder.obj();
}

// This is necessary because the run method must be scheduled in the Replication Executor
// for correct concurrency operation.
void ElectCmdRunnerTest::electCmdRunnerRunner(const executor::TaskExecutor::CallbackArgs& data,
                                              ElectCmdRunner* electCmdRunner,
                                              StatusWith<executor::TaskExecutor::EventHandle>* evh,
                                              const ReplSetConfig& currentConfig,
                                              int selfIndex,
                                              const std::vector<HostAndPort>& hosts) {
    invariant(data.status.isOK());
    *evh = electCmdRunner->start(data.executor, currentConfig, selfIndex, hosts);
}

void ElectCmdRunnerTest::startTest(ElectCmdRunner* electCmdRunner,
                                   const ReplSetConfig& currentConfig,
                                   int selfIndex,
                                   const std::vector<HostAndPort>& hosts) {
    StatusWith<executor::TaskExecutor::EventHandle> evh(ErrorCodes::InternalError, "Not set");
    StatusWith<executor::TaskExecutor::CallbackHandle> cbh = getExecutor().scheduleWork([&](
        const executor::TaskExecutor::CallbackArgs& data) {
        return electCmdRunnerRunner(data, electCmdRunner, &evh, currentConfig, selfIndex, hosts);
    });
    ASSERT_OK(cbh.getStatus());
    getExecutor().wait(cbh.getValue());
    ASSERT_OK(evh.getStatus());
    _allDoneEvent = evh.getValue();
}

void ElectCmdRunnerTest::waitForTest() {
    getExecutor().waitForEvent(_allDoneEvent);
}

TEST_F(ElectCmdRunnerTest, OneNode) {
    // Only one node in the config.
    const ReplSetConfig config = assertMakeRSConfig(BSON("_id"
                                                         << "rs0"
                                                         << "version"
                                                         << 1
                                                         << "protocolVersion"
                                                         << 1
                                                         << "members"
                                                         << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                                  << "h1"))));

    std::vector<HostAndPort> hosts;
    ElectCmdRunner electCmdRunner;
    startTest(&electCmdRunner, config, 0, hosts);
    waitForTest();
    ASSERT_EQUALS(electCmdRunner.getReceivedVotes(), 1);
}

TEST_F(ElectCmdRunnerTest, TwoNodes) {
    // Two nodes, we are node h1.
    const ReplSetConfig config = assertMakeRSConfig(BSON("_id"
                                                         << "rs0"
                                                         << "version"
                                                         << 1
                                                         << "protocolVersion"
                                                         << 1
                                                         << "members"
                                                         << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                                  << "h0")
                                                                       << BSON("_id" << 2 << "host"
                                                                                     << "h1"))));

    std::vector<HostAndPort> hosts;
    hosts.push_back(config.getMemberAt(1).getHostAndPort());

    const BSONObj electRequest = makeElectRequest(config, 0);

    ElectCmdRunner electCmdRunner;
    startTest(&electCmdRunner, config, 0, hosts);
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    ASSERT_EQUALS("admin", noi->getRequest().dbname);
    ASSERT_BSONOBJ_EQ(stripRound(electRequest), stripRound(noi->getRequest().cmdObj));
    ASSERT_EQUALS(HostAndPort("h1"), noi->getRequest().target);
    getNet()->scheduleResponse(
        noi,
        startDate + Milliseconds(10),
        (RemoteCommandResponse(BSON("ok" << 1 << "vote" << 1 << "round" << 380865962699346850ll),
                               BSONObj(),
                               Milliseconds(8))));
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), getNet()->now());
    waitForTest();
    ASSERT_EQUALS(electCmdRunner.getReceivedVotes(), 2);
}

TEST_F(ElectCmdRunnerTest, ShuttingDown) {
    // Two nodes, we are node h1.  Shutdown happens while we're scheduling remote commands.
    ReplSetConfig config = assertMakeRSConfig(BSON("_id"
                                                   << "rs0"
                                                   << "version"
                                                   << 1
                                                   << "protocolVersion"
                                                   << 1
                                                   << "members"
                                                   << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                            << "h0")
                                                                 << BSON("_id" << 2 << "host"
                                                                               << "h1"))));

    std::vector<HostAndPort> hosts;
    hosts.push_back(config.getMemberAt(1).getHostAndPort());

    ElectCmdRunner electCmdRunner;
    StatusWith<executor::TaskExecutor::EventHandle> evh(ErrorCodes::InternalError, "Not set");
    StatusWith<executor::TaskExecutor::CallbackHandle> cbh =
        getExecutor().scheduleWork([&](const executor::TaskExecutor::CallbackArgs& data) {
            return electCmdRunnerRunner(data, &electCmdRunner, &evh, config, 0, hosts);
        });
    ASSERT_OK(cbh.getStatus());
    getExecutor().wait(cbh.getValue());
    ASSERT_OK(evh.getStatus());
    shutdownExecutorThread();
    joinExecutorThread();
    getExecutor().waitForEvent(evh.getValue());
    ASSERT_EQUALS(electCmdRunner.getReceivedVotes(), 1);
}

class ElectScatterGatherTest : public mongo::unittest::Test {
public:
    virtual void start(const BSONObj& configObj) {
        int selfConfigIndex = 0;

        ReplSetConfig config;
        config.initialize(configObj).transitional_ignore();

        std::vector<HostAndPort> hosts;
        for (ReplSetConfig::MemberIterator mem = ++config.membersBegin();
             mem != config.membersEnd();
             ++mem) {
            hosts.push_back(mem->getHostAndPort());
        }

        _checker.reset(new ElectCmdRunner::Algorithm(config, selfConfigIndex, hosts, OID()));
    }

    virtual void tearDown() {
        _checker.reset(NULL);
    }

protected:
    bool hasReceivedSufficientResponses() {
        return _checker->hasReceivedSufficientResponses();
    }

    int getReceivedVotes() {
        return _checker->getReceivedVotes();
    }

    void processResponse(const RemoteCommandRequest& request,
                         const RemoteCommandResponse& response) {
        _checker->processResponse(request, response);
    }

    RemoteCommandRequest requestFrom(std::string hostname) {
        return RemoteCommandRequest(HostAndPort(hostname),
                                    "",  // the non-hostname fields do not matter for Elect
                                    BSONObj(),
                                    nullptr,
                                    Milliseconds(0));
    }

    RemoteCommandResponse badRemoteCommandResponse() {
        return RemoteCommandResponse(ErrorCodes::NodeNotFound, "not on my watch");
    }

    RemoteCommandResponse wrongTypeForVoteField() {
        return RemoteCommandResponse(NetworkInterfaceMock::Response(
            BSON("vote" << std::string("yea")), BSONObj(), Milliseconds(10)));
    }

    RemoteCommandResponse voteYea() {
        return RemoteCommandResponse(
            NetworkInterfaceMock::Response(BSON("vote" << 1), BSONObj(), Milliseconds(10)));
    }

    RemoteCommandResponse voteNay() {
        return RemoteCommandResponse(
            NetworkInterfaceMock::Response(BSON("vote" << -10000), BSONObj(), Milliseconds(10)));
    }

    RemoteCommandResponse abstainFromVoting() {
        return RemoteCommandResponse(
            NetworkInterfaceMock::Response(BSON("vote" << 0), BSONObj(), Milliseconds(10)));
    }

    BSONObj threeNodesTwoArbitersConfig() {
        return BSON("_id"
                    << "rs0"
                    << "version"
                    << 1
                    << "protocolVersion"
                    << 1
                    << "members"
                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                             << "host0")
                                  << BSON("_id" << 1 << "host"
                                                << "host1"
                                                << "arbiterOnly"
                                                << true)
                                  << BSON("_id" << 2 << "host"
                                                << "host2"
                                                << "arbiterOnly"
                                                << true)));
    }

    BSONObj basicThreeNodeConfig() {
        return BSON("_id"
                    << "rs0"
                    << "version"
                    << 1
                    << "protocolVersion"
                    << 1
                    << "members"
                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                             << "host0")
                                  << BSON("_id" << 1 << "host"
                                                << "host1")
                                  << BSON("_id" << 2 << "host"
                                                << "host2")));
    }

private:
    unique_ptr<ElectCmdRunner::Algorithm> _checker;
};

TEST_F(ElectScatterGatherTest, NodeRespondsWithBadVoteType) {
    start(basicThreeNodeConfig());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), wrongTypeForVoteField());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1, getReceivedVotes());  // 1 because we have 1 vote and voted for ourself
}

TEST_F(ElectScatterGatherTest, NodeRespondsWithBadStatus) {
    start(basicThreeNodeConfig());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), badRemoteCommandResponse());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host3"), abstainFromVoting());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1, getReceivedVotes());  // 1 because we have 1 vote and voted for ourself
}

TEST_F(ElectScatterGatherTest, FirstNodeRespondsWithYea) {
    start(basicThreeNodeConfig());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), voteYea());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(2, getReceivedVotes());
}

TEST_F(ElectScatterGatherTest, FirstNodeRespondsWithNaySecondWithYea) {
    start(basicThreeNodeConfig());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), voteNay());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(-9999, getReceivedVotes());
}

TEST_F(ElectScatterGatherTest, BothNodesAbstainFromVoting) {
    start(basicThreeNodeConfig());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), abstainFromVoting());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host3"), abstainFromVoting());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1, getReceivedVotes());
}

TEST_F(ElectScatterGatherTest, NodeRespondsWithBadStatusArbiters) {
    start(threeNodesTwoArbitersConfig());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), badRemoteCommandResponse());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host3"), abstainFromVoting());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1, getReceivedVotes());  // 1 because we have 1 vote and voted for ourself
}

TEST_F(ElectScatterGatherTest, FirstNodeRespondsWithYeaArbiters) {
    start(threeNodesTwoArbitersConfig());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), voteYea());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(2, getReceivedVotes());
}

TEST_F(ElectScatterGatherTest, FirstNodeRespondsWithNaySecondWithYeaArbiters) {
    start(threeNodesTwoArbitersConfig());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), voteNay());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(-9999, getReceivedVotes());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
