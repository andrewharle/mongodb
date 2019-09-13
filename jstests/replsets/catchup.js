// Test the catch-up behavior of new primaries.

(function() {
    "use strict";

    load("jstests/libs/check_log.js");
    load("jstests/libs/write_concern_util.js");
    load("jstests/replsets/rslib.js");

    var name = "catch_up";
    var rst = new ReplSetTest({name: name, nodes: 3, useBridge: true, waitForKeys: true});

    rst.startSet();
    var conf = rst.getReplSetConfig();
    conf.members[2].priority = 0;
    conf.settings = {
        heartbeatIntervalMillis: 500,
        electionTimeoutMillis: 10000,
        catchUpTimeoutMillis: 4 * 60 * 1000
    };
    rst.initiate(conf);
    rst.awaitSecondaryNodes();

    var primary = rst.getPrimary();
    var primaryColl = primary.getDB("test").coll;

    // Set verbosity for replication on all nodes.
    var verbosity = {
        "setParameter": 1,
        "logComponentVerbosity": {
            "replication": {"verbosity": 2},
        }
    };
    rst.nodes.forEach(function(node) {
        node.adminCommand(verbosity);
    });

    function stepUpNode(node) {
        assert.soonNoExcept(function() {
            assert.commandWorked(node.adminCommand({replSetStepUp: 1}));
            rst.awaitNodesAgreeOnPrimary(rst.kDefaultTimeoutMS, rst.nodes, rst.getNodeId(node));
            return node.adminCommand('replSetGetStatus').myState == ReplSetTest.State.PRIMARY;
        }, 'failed to step up node ' + node.host, rst.kDefaultTimeoutMS);

        return node;
    }

    function checkOpInOplog(node, op, count) {
        node.getDB("admin").getMongo().setSlaveOk();
        var oplog = node.getDB("local")['oplog.rs'];
        var oplogArray = oplog.find().toArray();
        assert.eq(oplog.count(op), count, "op: " + tojson(op) + ", oplog: " + tojson(oplogArray));
    }

    // Stop replication on secondaries, do writes and step up one of the secondaries.
    //
    // The old primary has extra writes that are not replicated to the other nodes yet,
    // but the new primary steps up, getting the vote from the the third node "voter".
    function stopReplicationAndEnforceNewPrimaryToCatchUp() {
        // Write documents that cannot be replicated to secondaries in time.
        var oldSecondaries = rst.getSecondaries();
        var oldPrimary = rst.getPrimary();
        stopServerReplication(oldSecondaries);
        for (var i = 0; i < 3; i++) {
            assert.writeOK(oldPrimary.getDB("test").foo.insert({x: i}));
        }
        var latestOpOnOldPrimary = getLatestOp(oldPrimary);
        // New primary wins immediately, but needs to catch up.
        var newPrimary = stepUpNode(oldSecondaries[0]);
        var latestOpOnNewPrimary = getLatestOp(newPrimary);
        // Check this node is not writable.
        assert.eq(newPrimary.getDB("test").isMaster().ismaster, false);

        return {
            oldSecondaries: oldSecondaries,
            oldPrimary: oldPrimary,
            newPrimary: newPrimary,
            voter: oldSecondaries[1],
            latestOpOnOldPrimary: latestOpOnOldPrimary,
            latestOpOnNewPrimary: latestOpOnNewPrimary
        };
    }

    function reconfigElectionAndCatchUpTimeout(electionTimeout, catchupTimeout) {
        // Reconnect all nodes to make sure reconfig succeeds.
        rst.nodes.forEach(reconnect);
        // Reconfigure replica set to decrease catchup timeout.
        var newConfig = rst.getReplSetConfigFromNode();
        newConfig.version++;
        newConfig.settings.catchUpTimeoutMillis = catchupTimeout;
        newConfig.settings.electionTimeoutMillis = electionTimeout;
        reconfig(rst, newConfig);
        rst.awaitReplication();
        rst.awaitNodesAgreeOnPrimary();
    }

    rst.awaitReplication();

    jsTest.log("Case 1: The primary is up-to-date after refreshing heartbeats.");
    // Should complete transition to primary immediately.
    var newPrimary = stepUpNode(rst.getSecondary());
    // Should win an election and finish the transition very quickly.
    assert.eq(newPrimary, rst.getPrimary());
    rst.awaitReplication();

    jsTest.log("Case 2: The primary needs to catch up, succeeds in time.");
    var stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp();

    // Disable fail point to allow replication.
    restartServerReplication(stepUpResults.oldSecondaries);
    // getPrimary() blocks until the primary finishes drain mode.
    assert.eq(stepUpResults.newPrimary, rst.getPrimary());
    // Wait for all secondaries to catch up
    rst.awaitReplication();
    // Check the latest op on old primary is preserved on the new one.
    checkOpInOplog(stepUpResults.newPrimary, stepUpResults.latestOpOnOldPrimary, 1);
    rst.awaitReplication();

    jsTest.log("Case 3: The primary needs to catch up, but has to change sync source to catch up.");
    // Reconfig the election timeout to be longer than 1 minute so that the third node will no
    // longer be blacklisted by the new primary if it happened to be at the beginning of the test.
    reconfigElectionAndCatchUpTimeout(3 * 60 * 1000, conf.settings.catchUpTimeoutMillis);

    stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp();

    // Disable fail point on the voter. Wait until it catches up with the old primary.
    restartServerReplication(stepUpResults.voter);
    assert.commandWorked(
        stepUpResults.voter.adminCommand({replSetSyncFrom: stepUpResults.oldPrimary.host}));
    // Wait until the new primary knows the last applied optime on the voter, so it will keep
    // catching up after the old primary is disconnected.
    assert.soon(function() {
        var replSetStatus =
            assert.commandWorked(stepUpResults.newPrimary.adminCommand({replSetGetStatus: 1}));
        var voterStatus = replSetStatus.members.filter(m => m.name == stepUpResults.voter.host)[0];
        return rs.compareOpTimes(voterStatus.optime, stepUpResults.latestOpOnOldPrimary) == 0;
    });
    // Disconnect the new primary and the old one.
    stepUpResults.oldPrimary.disconnect(stepUpResults.newPrimary);
    // Disable the failpoint, the new primary should sync from the other secondary.
    restartServerReplication(stepUpResults.newPrimary);
    assert.eq(stepUpResults.newPrimary, rst.getPrimary());
    checkOpInOplog(stepUpResults.newPrimary, stepUpResults.latestOpOnOldPrimary, 1);
    // Restore the broken connection
    stepUpResults.oldPrimary.reconnect(stepUpResults.newPrimary);
    rst.awaitReplication();

    jsTest.log("Case 4: The primary needs to catch up, fails due to timeout.");
    // Reconfig to make the catchup timeout shorter.
    reconfigElectionAndCatchUpTimeout(conf.settings.electionTimeoutMillis, 10 * 1000);

    stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp();
    // Wait until the new primary completes the transition to primary and writes a no-op.
    checkLog.contains(stepUpResults.newPrimary, "Catchup timed out after becoming primary");
    restartServerReplication(stepUpResults.newPrimary);
    assert.eq(stepUpResults.newPrimary, rst.getPrimary());

    // Wait for the no-op "new primary" after winning an election, so that we know it has
    // finished transition to primary.
    assert.soon(function() {
        return rs.compareOpTimes(stepUpResults.latestOpOnOldPrimary,
                                 getLatestOp(stepUpResults.newPrimary)) < 0;
    });
    // The extra oplog entries on the old primary are not replicated to the new one.
    checkOpInOplog(stepUpResults.newPrimary, stepUpResults.latestOpOnOldPrimary, 0);
    restartServerReplication(stepUpResults.voter);
    rst.awaitReplication();

    jsTest.log("Case 5: The primary needs to catch up with no timeout, then gets aborted.");
    // Reconfig to make the catchup timeout infinite.
    reconfigElectionAndCatchUpTimeout(conf.settings.electionTimeoutMillis, -1);
    stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp();

    // Abort catchup.
    assert.commandWorked(stepUpResults.newPrimary.adminCommand({replSetAbortPrimaryCatchUp: 1}));

    // Wait for the no-op "new primary" after winning an election, so that we know it has
    // finished transition to primary.
    assert.soon(function() {
        return rs.compareOpTimes(stepUpResults.latestOpOnOldPrimary,
                                 getLatestOp(stepUpResults.newPrimary)) < 0;
    });
    // The extra oplog entries on the old primary are not replicated to the new one.
    checkOpInOplog(stepUpResults.newPrimary, stepUpResults.latestOpOnOldPrimary, 0);
    restartServerReplication(stepUpResults.oldSecondaries);
    rst.awaitReplication();
    checkOpInOplog(stepUpResults.newPrimary, stepUpResults.latestOpOnOldPrimary, 0);

    jsTest.log("Case 6: The primary needs to catch up with no timeout, but steps down.");
    var stepUpResults = stopReplicationAndEnforceNewPrimaryToCatchUp();

    // Step-down command should abort catchup.
    try {
        printjson(stepUpResults.newPrimary.adminCommand({replSetStepDown: 60}));
    } catch (e) {
        print(e);
    }
    // Rename the primary.
    var steppedDownPrimary = stepUpResults.newPrimary;
    var newPrimary = rst.getPrimary();
    assert.neq(newPrimary, steppedDownPrimary);

    // Enable data replication on the stepped down primary and make sure it syncs old writes.
    rst.nodes.forEach(reconnect);
    restartServerReplication(stepUpResults.oldSecondaries);
    rst.awaitReplication();
    checkOpInOplog(steppedDownPrimary, stepUpResults.latestOpOnOldPrimary, 1);

    rst.stopSet();
})();
