
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replication_coordinator.h"  // ReplSetReconfigArgs
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

typedef ReplicationCoordinator::ReplSetReconfigArgs ReplSetReconfigArgs;

TEST_F(ReplCoordTest, NodeReturnsNotYetInitializedWhenReconfigReceivedPriorToInitialization) {
    // start up but do not initiate
    init();
    start();
    BSONObjBuilder result;
    ReplSetReconfigArgs args;

    const auto opCtx = makeOperationContext();
    ASSERT_EQUALS(ErrorCodes::NotYetInitialized,
                  getReplCoord()->processReplSetReconfig(opCtx.get(), args, &result));
    ASSERT_TRUE(result.obj().isEmpty());
}

TEST_F(ReplCoordTest, NodeReturnsNotMasterWhenReconfigReceivedWhileSecondary) {
    // start up, become secondary, receive reconfig
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));

    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));

    BSONObjBuilder result;
    ReplSetReconfigArgs args;
    args.force = false;
    const auto opCtx = makeOperationContext();
    ASSERT_EQUALS(ErrorCodes::NotMaster,
                  getReplCoord()->processReplSetReconfig(opCtx.get(), args, &result));
    ASSERT_TRUE(result.obj().isEmpty());
}

TEST_F(ReplCoordTest, NodeReturnsInvalidReplicaSetConfigWhenReconfigReceivedWithInvalidConfig) {
    // start up, become primary, receive uninitializable config
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();

    BSONObjBuilder result;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 2
                             << "protocolVersion"
                             << 1
                             << "invalidlyNamedField"
                             << 3
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "arbiterOnly"
                                                      << true)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345"
                                                         << "arbiterOnly"
                                                         << true)));
    const auto opCtx = makeOperationContext();
    // ErrorCodes::BadValue should be propagated from ReplSetConfig::initialize()
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  getReplCoord()->processReplSetReconfig(opCtx.get(), args, &result));
    ASSERT_TRUE(result.obj().isEmpty());
}

TEST_F(ReplCoordTest, NodeReturnsInvalidReplicaSetConfigWhenReconfigReceivedWithIncorrectSetName) {
    // start up, become primary, receive config with incorrect replset name
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();

    BSONObjBuilder result;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "notMySet"
                             << "version"
                             << 3
                             << "protocolVersion"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")));

    const auto opCtx = makeOperationContext();
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  getReplCoord()->processReplSetReconfig(opCtx.get(), args, &result));
    ASSERT_TRUE(result.obj().isEmpty());
}

TEST_F(ReplCoordTest, NodeReturnsInvalidReplicaSetConfigWhenReconfigReceivedWithIncorrectSetId) {
    // start up, become primary, receive config with incorrect replset name
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))
                            << "settings"
                            << BSON("replicaSetId" << OID::gen())),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();

    BSONObjBuilder result;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 3
                             << "protocolVersion"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345"))
                             << "settings"
                             << BSON("replicaSetId" << OID::gen()));

    const auto opCtx = makeOperationContext();
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  getReplCoord()->processReplSetReconfig(opCtx.get(), args, &result));
    ASSERT_TRUE(result.obj().isEmpty());
}

TEST_F(ReplCoordTest,
       NodeReturnsNewReplicaSetConfigurationIncompatibleWhenANewConfigFailsToValidate) {
    // start up, become primary, validate fails
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();

    BSONObjBuilder result;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << -3
                             << "protocolVersion"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")));

    const auto opCtx = makeOperationContext();
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                  getReplCoord()->processReplSetReconfig(opCtx.get(), args, &result));
    ASSERT_TRUE(result.obj().isEmpty());
}

void doReplSetInitiate(ReplicationCoordinatorImpl* replCoord,
                       Status* status,
                       OperationContext* opCtx) {
    BSONObjBuilder garbage;
    *status =
        replCoord->processReplSetInitiate(opCtx,
                                          BSON("_id"
                                               << "mySet"
                                               << "version"
                                               << 1
                                               << "members"
                                               << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                        << "node1:12345")
                                                             << BSON("_id" << 2 << "host"
                                                                           << "node2:12345"))),
                                          &garbage);
}

void doReplSetReconfig(ReplicationCoordinatorImpl* replCoord,
                       Status* status,
                       OperationContext* opCtx) {
    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = false;
    // Replica set id will be copied from existing configuration.
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 3
                             << "protocolVersion"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345"
                                                         << "priority"
                                                         << 3)));
    *status = replCoord->processReplSetReconfig(opCtx, args, &garbage);
}

TEST_F(ReplCoordTest,
       NodeReturnsNewReplicaSetConfigurationIncompatibleWhenQuorumCheckFailsDuringReconfig) {
    // start up, become primary, fail during quorum check due to a heartbeat
    // containing a higher config version
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();

    Status status(ErrorCodes::InternalError, "Not Set");
    const auto opCtx = makeOperationContext();
    stdx::thread reconfigThread([&] { doReplSetReconfig(getReplCoord(), &status, opCtx.get()); });

    NetworkInterfaceMock* net = getNet();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    repl::ReplSetHeartbeatArgsV1 hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    repl::ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setConfigVersion(5);
    BSONObjBuilder respObj;
    respObj << "ok" << 1;
    hbResp.addToBSON(&respObj, false);
    net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
    net->runReadyNetworkOperations();
    getNet()->exitNetwork();
    reconfigThread.join();
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
}

TEST_F(ReplCoordTest, NodeReturnsOutOfDiskSpaceWhenSavingANewConfigFailsDuringReconfig) {
    // start up, become primary, saving the config fails
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();

    Status status(ErrorCodes::InternalError, "Not Set");
    getExternalState()->setStoreLocalConfigDocumentStatus(
        Status(ErrorCodes::OutOfDiskSpace, "The test set this"));
    const auto opCtx = makeOperationContext();
    stdx::thread reconfigThread([&] { doReplSetReconfig(getReplCoord(), &status, opCtx.get()); });

    replyToReceivedHeartbeatV1();
    reconfigThread.join();
    ASSERT_EQUALS(ErrorCodes::OutOfDiskSpace, status);
}

TEST_F(ReplCoordTest,
       NodeReturnsConfigurationInProgressWhenReceivingAReconfigWhileInTheMidstOfAnotherReconfig) {
    // start up, become primary, reconfig, then before that reconfig concludes, reconfig again
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();

    Status status(ErrorCodes::InternalError, "Not Set");
    const auto opCtx = makeOperationContext();
    // first reconfig
    stdx::thread reconfigThread([&] { doReplSetReconfig(getReplCoord(), &status, opCtx.get()); });
    getNet()->enterNetwork();
    getNet()->blackHole(getNet()->getNextReadyRequest());
    getNet()->exitNetwork();

    // second reconfig
    BSONObjBuilder result;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 3
                             << "protocolVersion"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")));
    ASSERT_EQUALS(ErrorCodes::ConfigurationInProgress,
                  getReplCoord()->processReplSetReconfig(opCtx.get(), args, &result));
    ASSERT_TRUE(result.obj().isEmpty());

    shutdown(opCtx.get());
    reconfigThread.join();
}

TEST_F(ReplCoordTest, NodeReturnsConfigurationInProgressWhenReceivingAReconfigWhileInitiating) {
    // start up, initiate, then before that initiate concludes, reconfig
    init();
    start(HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));

    // initiate
    Status status(ErrorCodes::InternalError, "Not Set");
    const auto opCtx = makeOperationContext();
    stdx::thread initateThread([&] { doReplSetInitiate(getReplCoord(), &status, opCtx.get()); });
    getNet()->enterNetwork();
    getNet()->blackHole(getNet()->getNextReadyRequest());
    getNet()->exitNetwork();

    // reconfig
    BSONObjBuilder result;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 3
                             << "protocolVersion"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")));
    ASSERT_EQUALS(ErrorCodes::ConfigurationInProgress,
                  getReplCoord()->processReplSetReconfig(opCtx.get(), args, &result));
    ASSERT_TRUE(result.obj().isEmpty());

    shutdown(opCtx.get());
    initateThread.join();
}

TEST_F(ReplCoordTest, PrimaryNodeAcceptsNewConfigWhenReceivingAReconfigWithACompatibleConfig) {
    // start up, become primary, reconfig successfully
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))
                            << "settings"
                            << BSON("replicaSetId" << OID::gen())),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();

    Status status(ErrorCodes::InternalError, "Not Set");
    const auto opCtx = makeOperationContext();
    stdx::thread reconfigThread([&] { doReplSetReconfig(getReplCoord(), &status, opCtx.get()); });

    NetworkInterfaceMock* net = getNet();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    repl::ReplSetHeartbeatArgsV1 hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    repl::ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setConfigVersion(2);
    BSONObjBuilder respObj;
    respObj << "ok" << 1;
    hbResp.addToBSON(&respObj, false);
    net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
    net->runReadyNetworkOperations();
    getNet()->exitNetwork();
    reconfigThread.join();
    ASSERT_OK(status);
}

TEST_F(
    ReplCoordTest,
    NodeReturnsConfigurationInProgressWhenReceivingAReconfigWhileInTheMidstOfAHeartbeatReconfig) {
    // start up, become primary, receive reconfig via heartbeat, then a second one
    // from reconfig
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    getGlobalFailPointRegistry()
        ->getFailPoint("blockHeartbeatReconfigFinish")
        ->setMode(FailPoint::alwaysOn);

    // hb reconfig
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    ReplSetHeartbeatResponse hbResp2;
    ReplSetConfig config;
    config
        .initialize(BSON("_id"
                         << "mySet"
                         << "version"
                         << 3
                         << "protocolVersion"
                         << 1
                         << "members"
                         << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                  << "node1:12345")
                                       << BSON("_id" << 2 << "host"
                                                     << "node2:12345"))))
        .transitional_ignore();
    hbResp2.setConfig(config);
    hbResp2.setConfigVersion(3);
    hbResp2.setSetName("mySet");
    hbResp2.setState(MemberState::RS_SECONDARY);
    BSONObjBuilder respObj2;
    respObj2 << "ok" << 1;
    hbResp2.addToBSON(&respObj2, false);
    net->runUntil(net->now() + Seconds(10));  // run until we've sent a heartbeat request
    const NetworkInterfaceMock::NetworkOperationIterator noi2 = net->getNextReadyRequest();
    net->scheduleResponse(noi2, net->now(), makeResponseStatus(respObj2.obj()));
    net->runReadyNetworkOperations();
    getNet()->exitNetwork();

    // reconfig
    BSONObjBuilder result;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = config.toBSON();
    const auto opCtx = makeOperationContext();
    ASSERT_EQUALS(ErrorCodes::ConfigurationInProgress,
                  getReplCoord()->processReplSetReconfig(opCtx.get(), args, &result));

    getGlobalFailPointRegistry()
        ->getFailPoint("blockHeartbeatReconfigFinish")
        ->setMode(FailPoint::off);
}

TEST_F(ReplCoordTest, NodeDoesNotAcceptHeartbeatReconfigWhileInTheMidstOfReconfig) {
    // start up, become primary, reconfig, while reconfigging receive reconfig via heartbeat
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // start reconfigThread
    Status status(ErrorCodes::InternalError, "Not Set");
    const auto opCtx = makeOperationContext();
    stdx::thread reconfigThread([&] { doReplSetReconfig(getReplCoord(), &status, opCtx.get()); });

    // wait for reconfigThread to create network requests to ensure the replication coordinator
    // is in state kConfigReconfiguring
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    net->blackHole(net->getNextReadyRequest());

    // schedule hb reconfig
    net->runUntil(net->now() + Seconds(10));  // run until we've sent a heartbeat request
    const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    ReplSetHeartbeatResponse hbResp;
    ReplSetConfig config;
    config
        .initialize(BSON("_id"
                         << "mySet"
                         << "version"
                         << 4
                         << "members"
                         << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                  << "node1:12345")
                                       << BSON("_id" << 2 << "host"
                                                     << "node2:12345"))))
        .transitional_ignore();
    hbResp.setConfig(config);
    hbResp.setConfigVersion(4);
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    BSONObjBuilder respObj2;
    respObj2 << "ok" << 1;
    hbResp.addToBSON(&respObj2, false);
    net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj2.obj()));

    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(1));
    startCapturingLogMessages();
    // execute hb reconfig, which should fail with a log message; confirmed at end of test
    net->runReadyNetworkOperations();
    // respond to reconfig's quorum check so that we can join that thread and exit cleanly
    net->exitNetwork();
    stopCapturingLogMessages();
    ASSERT_EQUALS(
        1, countLogLinesContaining("because already in the midst of a configuration process"));
    shutdown(opCtx.get());
    reconfigThread.join();
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Log());
}

TEST_F(ReplCoordTest, NodeAcceptsConfigFromAReconfigWithForceTrueWhileNotPrimary) {
    // start up, become a secondary, receive a forced reconfig
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));

    // fail before forced
    BSONObjBuilder result;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 3
                             << "protocolVersion"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")));
    const auto opCtx = makeOperationContext();
    ASSERT_EQUALS(ErrorCodes::NotMaster,
                  getReplCoord()->processReplSetReconfig(opCtx.get(), args, &result));

    // forced should succeed
    args.force = true;
    ASSERT_OK(getReplCoord()->processReplSetReconfig(opCtx.get(), args, &result));
    getReplCoord()->processReplSetGetConfig(&result);

    // ensure forced reconfig results in a random larger version
    ASSERT_GREATER_THAN(result.obj()["config"].Obj()["version"].numberInt(), 3);
}

}  // anonymous namespace
}  // namespace repl
}  // namespace mongo
