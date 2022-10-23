/**
 * DEPRECATED (SERVER-35002): RollbackTest is deprecated. Please use RollbackTestDeluxe instead.
 *
 * Wrapper around ReplSetTest for testing rollback behavior. It allows the caller to easily
 * transition between stages of a rollback without having to manually operate on the replset.
 *
 * This library exposes the following 5 sequential stages of rollback:
 * 1. RollbackTest starts in kSteadyStateOps: the replica set is in steady state replication.
 *        Operations applied will be replicated.
 * 2. kRollbackOps: operations applied during this phase will not be replicated and eventually be
 *        rolled back.
 * 3. kSyncSourceOpsBeforeRollback: apply operations on the sync source before rollback begins.
 * 4. kSyncSourceOpsDuringRollback: apply operations on the sync source after rollback has begun.
 * 5. kSteadyStateOps: (same as stage 1) with the option of waiting for the rollback to finish.
 *
 * --------------------------------------------------
 * | STATE TRANSITION            | NETWORK TOPOLOGY |
 * |-------------------------------------------------
 * |  kSteadyStateOps            |       A          |
 * |                             |     /   \        |
 * |                             |    P1 -  S       |
 * |-----------------------------|------------------|
 * |  kRollbackOps               |       A          |
 * |                             |     /            |
 * |                             |    P1    S       |
 * |-----------------------------|------------------|
 * | kSyncSourceOpsBeforeRollback|       A          |
 * |                             |         \        |
 * |                             |    P1    P2      |
 * |-----------------------------|------------------|
 * | kSyncSourceOpsDuringRollback|        A         |
 * |                             |      /   \       |
 * |                             |     R  -  P2     |
 * |-------------------------------------------------
 * Note: 'A' refers to arbiter node, 'S' refers to secondary, 'P[n]' refers to primary in
 * nth term and 'R' refers to rollback node.
 *
 * Please refer to the various `transition*` functions for more information on the behavior
 * of each stage.
 */

"use strict";

load("jstests/replsets/rslib.js");
load("jstests/replsets/libs/two_phase_drops.js");
load("jstests/hooks/validate_collections.js");

/**
 * DEPRECATED (SERVER-35002): RollbackTest is deprecated. Please use RollbackTestDeluxe instead.
 *
 * This fixture allows the user to optionally pass in a custom ReplSetTest
 * to be used for the test. The underlying replica set must meet the following
 * requirements:
 *      1. It must have exactly three nodes: a primary, a secondary and an arbiter.
 *      2. It must be running with mongobridge.
 *      3. Must initiate the replset with high election timeout to avoid unplanned elections in the
 *         rollback test.
 *
 * If the caller does not provide their own replica set, a standard three-node
 * replset will be initialized instead, with all nodes running the latest version.
 *
 * @param {string} [optional] name the name of the test being run
 * @param {Object} [optional] replSet the ReplSetTest instance to adopt
 */
function RollbackTest(name = "RollbackTest", replSet) {
    const State = {
        kStopped: "kStopped",
        kRollbackOps: "kRollbackOps",
        kSyncSourceOpsBeforeRollback: "kSyncSourceOpsBeforeRollback",
        kSyncSourceOpsDuringRollback: "kSyncSourceOpsDuringRollback",
        kSteadyStateOps: "kSteadyStateOps",
    };

    const AcceptableTransitions = {
        [State.kStopped]: [],
        [State.kRollbackOps]: [State.kSyncSourceOpsBeforeRollback],
        [State.kSyncSourceOpsBeforeRollback]: [State.kSyncSourceOpsDuringRollback],
        [State.kSyncSourceOpsDuringRollback]: [State.kSteadyStateOps],
        [State.kSteadyStateOps]: [State.kStopped, State.kRollbackOps],
    };

    const collectionValidator = new CollectionValidator();

    const SIGKILL = 9;
    const SIGTERM = 15;
    const kNumDataBearingNodes = 2;

    let awaitSecondaryNodesForRollbackTimeout;

    let rst;
    let curPrimary;
    let curSecondary;
    let arbiter;

    let curState = State.kSteadyStateOps;
    let lastRBID;

    // Make sure we have a replica set up and running.
    replSet = (replSet === undefined) ? performStandardSetup() : replSet;
    validateAndUseSetup(replSet);

    /**
     * Validate and use the provided replica set.
     *
     * @param {Object} replSet the ReplSetTest instance to adopt
     */
    function validateAndUseSetup(replSet) {
        assert.eq(true,
                  replSet instanceof ReplSetTest,
                  `Must provide an instance of ReplSetTest. Have: ${tojson(replSet)}`);

        assert.eq(true, replSet.usesBridge(), "Must set up ReplSetTest with mongobridge enabled.");
        assert.eq(3, replSet.nodes.length, "Replica set must contain exactly three nodes.");

        // Make sure we have a primary.
        curPrimary = replSet.getPrimary();

        // Extract the other two nodes and wait for them to be ready.
        let secondaries = replSet.getSecondaries();
        arbiter = replSet.getArbiter();
        curSecondary = (secondaries[0] === arbiter) ? secondaries[1] : secondaries[0];

        let config = replSet.getReplSetConfigFromNode();
        // Make sure electionTimeoutMillis is set to high value to avoid unplanned elections in
        // the rollback test.
        assert.gte(config.settings.electionTimeoutMillis,
                   ReplSetTest.kForeverMillis,
                   "Must initiate the replset with high election timeout");

        waitForState(curSecondary, ReplSetTest.State.SECONDARY);
        waitForState(arbiter, ReplSetTest.State.ARBITER);

        rst = replSet;
        lastRBID = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;
    }

    /**
     * Return an instance of ReplSetTest initialized with a standard
     * three-node replica set running with the latest version.
     */
    function performStandardSetup() {
        let nodeOptions = {};
        if (TestData.logComponentVerbosity) {
            nodeOptions["setParameter"] = {
                "logComponentVerbosity": tojsononeline(TestData.logComponentVerbosity)
            };
        }
        if (TestData.syncdelay) {
            nodeOptions["syncdelay"] = TestData.syncdelay;
        }

        let replSet = new ReplSetTest({name, nodes: 3, useBridge: true, nodeOptions: nodeOptions});
        replSet.startSet();

        const nodes = replSet.nodeList();
        replSet.initiateWithHighElectionTimeout({
            _id: name,
            members: [
                {_id: 0, host: nodes[0]},
                {_id: 1, host: nodes[1]},
                {_id: 2, host: nodes[2], arbiterOnly: true}
            ]
        });

        assert.eq(replSet.nodes.length - replSet.getArbiters().length,
                  kNumDataBearingNodes,
                  "Mismatch between number of data bearing nodes and test configuration.");

        return replSet;
    }

    function checkDataConsistency() {
        assert.eq(curState,
                  State.kSteadyStateOps,
                  "Not in kSteadyStateOps state, cannot check data consistency");

        // We must wait for collection drops to complete so that we don't get spurious failures
        // in the consistency checks.
        rst.nodes.forEach(node => {
            if (node.getDB('admin').isMaster('admin').arbiterOnly === true) {
                log(`Skipping waiting for collection drops on arbiter ${node.host}`);
                return;
            }
            TwoPhaseDropCollectionTest.waitForAllCollectionDropsToComplete(node);
        });

        const name = rst.name;
        // We must check counts before we validate since validate fixes counts. We cannot check
        // counts if unclean shutdowns occur.
        if (!TestData.allowUncleanShutdowns || !TestData.rollbackShutdowns) {
            rst.checkCollectionCounts(name);
        }
        rst.checkOplogs(name);
        rst.checkReplicatedDataHashes(name);
        collectionValidator.validateNodes(rst.nodeList());
    }

    function log(msg, important = false) {
        if (important) {
            jsTestLog(`[${name}] ${msg}`);
        } else {
            print(`[${name}] ${msg}`);
        }
    }

    /**
     * return whether the cluster can transition from the current State to `newState`.
     * @private
     */
    function transitionIfAllowed(newState) {
        if (AcceptableTransitions[curState].includes(newState)) {
            log(`Transitioning to: "${newState}"`, true);
            curState = newState;
        } else {
            // Transitioning to a disallowed State is likely a bug in the code, so we throw an
            // error here instead of silently failing.
            throw new Error(`Can't transition to State "${newState}" from State "${curState}"`);
        }
    }

    function stepUp(conn) {
        log(`Waiting for the new primary ${conn.host} to be elected`);
        assert.soonNoExcept(() => {
            const res = conn.adminCommand({replSetStepUp: 1});
            return res.ok;
        });

        // Waits for the primary to accept new writes.
        return rst.getPrimary();
    }

    function oplogTop(conn) {
        return conn.getDB("local").oplog.rs.find().limit(1).sort({$natural: -1}).next();
    }

    /**
     * Transition from a rollback state to a steady state. Operations applied in this phase will
     * be replicated to all nodes and should not be rolled back.
     */
    this.transitionToSteadyStateOperations = function() {
        // Ensure the secondary is connected. It may already have been connected from a previous
        // stage.
        log(`Ensuring the secondary ${curSecondary.host} is connected to the other nodes`);
        curSecondary.reconnect([curPrimary, arbiter]);

        // 1. Wait for the rollback node to be SECONDARY; this either waits for rollback to finish
        // or exits early if it checks the node before it *enters* ROLLBACK.
        //
        // 2. Test that RBID is properly incremented; note that it could be incremented several
        // times if the node restarts before a given rollback attempt finishes.
        //
        // 3. Check if the rollback node is caught up.
        //
        // If any conditions are unmet, retry.
        //
        // If {enableMajorityReadConcern:false} is set, it will use the rollbackViaRefetch
        // algorithm. That can lead to unrecoverable rollbacks, particularly in unclean shutdown
        // suites, as it is possible in rare cases for the sync source to lose the entry
        // corresponding to the optime the rollback node chose as its minValid.

        log(`Wait for ${curSecondary.host} to finish rollback`);
        assert.soonNoExcept(() => {
            try {
                log(`Wait for secondary ${curSecondary}`);
                rst.awaitSecondaryNodesForRollbackTest(
                    awaitSecondaryNodesForRollbackTimeout,
                    curSecondary /* connToCheckForUnrecoverableRollback */);
            } catch (e) {
                if (e.unrecoverableRollbackDetected) {
                    log(`Detected unrecoverable rollback on ${curSecondary.host}. Ending test.`,
                        true /* important */);
                    TestData.skipCheckDBHashes = true;
                    rst.stopSet();
                    quit();
                }
                // Re-throw the original exception in all other cases.
                throw e;
            }

            let rbid = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;
            assert(rbid > lastRBID,
                   `Expected RBID to increment past ${lastRBID} on ${curSecondary.host}`);

            assert.eq(oplogTop(curPrimary), oplogTop(curSecondary));

            return true;
        });

        rst.awaitReplication();

        log(`Rollback on ${curSecondary.host} (if needed) and awaitReplication completed`, true);

        // We call transition to steady state ops after awaiting replication has finished,
        // otherwise it could be confusing to see operations being replicated when we're already
        // in rollback complete state.
        transitionIfAllowed(State.kSteadyStateOps);

        // After the previous rollback (if any) has completed and await replication has finished,
        // the replica set should be in a consistent and "fresh" state. We now prepare for the next
        // rollback.
        checkDataConsistency();

        return curPrimary;
    };

    /**
     * Transition to the first stage of rollback testing, where we isolate the current primary so
     * its operations will eventually be rolled back.
     */
    this.transitionToRollbackOperations = function() {
        // Ensure previous operations are replicated. The current secondary will be used as the sync
        // source later on, so it must be up-to-date to prevent any previous operations from being
        // rolled back.
        rst.awaitSecondaryNodes();
        rst.awaitReplication();

        transitionIfAllowed(State.kRollbackOps);

        // Disconnect the current primary from the secondary so operations on it will eventually be
        // rolled back. But do not disconnect it from the arbiter so it can stay as the primary.
        log(`Isolating the primary ${curPrimary.host} from the secondary ${curSecondary.host}`);
        curPrimary.disconnect([curSecondary]);

        return curPrimary;
    };

    /**
     * Insert on primary until its lastApplied >= the rollback node's. Useful for testing rollback
     * via refetch, which completes rollback recovery when new lastApplied >= old top of oplog.
     */
    this.awaitPrimaryAppliedSurpassesRollbackApplied = function() {
        log(`Waiting for lastApplied on sync source ${curPrimary.host} to surpass lastApplied` +
            ` on rollback node ${curSecondary.host}`);

        function lastApplied(node) {
            const reply = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
            return reply.optimes.appliedOpTime.ts;
        }

        const rollbackApplied = lastApplied(curSecondary);
        assert.soon(() => {
            const primaryApplied = lastApplied(curPrimary);
            jsTestLog(
                `lastApplied on sync source ${curPrimary.host}:` +
                ` ${tojson(primaryApplied)}, lastApplied on rollback node ${curSecondary.host}:` +
                ` ${tojson(rollbackApplied)}`);

            if (timestampCmp(primaryApplied, rollbackApplied) >= 0) {
                return true;
            }

            let crudColl = curPrimary.getDB("test")["awaitPrimaryAppliedSurpassesRollbackApplied"];
            assert.commandWorked(crudColl.insertOne({}));
        }, "primary's lastApplied never surpassed rollback node's");
    };

    /**
     * Transition to the second stage of rollback testing, where we isolate the old primary and
     * elect the old secondary as the new primary. Then, operations can be performed on the new
     * primary so that that optimes diverge and previous operations on the old primary will be
     * rolled back.
     */
    this.transitionToSyncSourceOperationsBeforeRollback = function() {
        transitionIfAllowed(State.kSyncSourceOpsBeforeRollback);

        // Insert one document to ensure rollback will not be skipped.
        let dbName = "EnsureThereIsAtLeastOneOperationToRollback";
        assert.writeOK(curPrimary.getDB(dbName).ensureRollback.insert(
            {thisDocument: 'is inserted to ensure rollback is not skipped'}));

        log(`Isolating the primary ${curPrimary.host} so it will step down`);
        curPrimary.disconnect([curSecondary, arbiter]);

        log(`Waiting for the primary ${curPrimary.host} to step down`);
        try {
            // The stepdown freeze period is short because the node is disconnected from
            // the rest of the replica set, so it physically can't become the primary.
            assert.soon(() => {
                const res = curPrimary.adminCommand({replSetStepDown: 1, force: true});
                return (res.ok || res.code === ErrorCodes.NotMaster);
            });
        } catch (e) {
            // Stepdown may fail if the node has already started stepping down.
            print('Caught exception from replSetStepDown: ' + e);
        }

        waitForState(curPrimary, ReplSetTest.State.SECONDARY);

        log(`Reconnecting the secondary ${curSecondary.host} to the arbiter so it can be elected`);
        curSecondary.reconnect([arbiter]);

        const newPrimary = stepUp(curSecondary);

        // As a sanity check, ensure the new primary is the old secondary. The opposite scenario
        // should never be possible with 2 electable nodes and the sequence of operations thus far.
        assert.eq(newPrimary, curSecondary, "Did not elect a new node as primary");
        log(`Elected the old secondary ${newPrimary.host} as the new primary`);

        // The old primary is the new secondary; the old secondary just got elected as the new
        // primary, so we update the topology to reflect this change.
        curSecondary = curPrimary;
        curPrimary = newPrimary;

        // To ensure rollback won't be skipped for shutdowns, wait till the no-op oplog
        // entry ("new primary") written in the new term gets persisted in the disk.
        // Note: rollbackShutdowns are not allowed for in-memory/ephemeral storage engines.
        if (TestData.rollbackShutdowns) {
            const dbName = "TermGetsPersisted";
            assert.commandWorked(curPrimary.getDB(dbName).ensureRollback.insert(
                {thisDocument: 'is inserted to ensure rollback is not skipped'},
                {writeConcern: {w: 1, j: true}}));
        }

        lastRBID = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;

        const isMajorityReadConcernEnabledOnRollbackNode =
            assert.commandWorked(curSecondary.adminCommand({serverStatus: 1}))
                .storageEngine.supportsCommittedReads;
        const isInMemoryStorageEngine = jsTest.options().storageEngine === "inMemory";
        if (!isMajorityReadConcernEnabledOnRollbackNode || isInMemoryStorageEngine) {
            this.awaitPrimaryAppliedSurpassesRollbackApplied();
        }

        return curPrimary;
    };

    /**
     * Transition to the third stage of rollback testing, where we reconnect the rollback node so
     * it will start rolling back.
     *
     * Note that there is no guarantee that operations performed on the sync source while in this
     * state will actually occur *during* the rollback process. They may happen before the rollback
     * is finished or after the rollback is done. We provide this state, though, as an attempt to
     * provide a way to test this behavior, even if it's non-deterministic.
     */
    this.transitionToSyncSourceOperationsDuringRollback = function() {
        transitionIfAllowed(State.kSyncSourceOpsDuringRollback);

        log(`Reconnecting the secondary ${curSecondary.host} so it'll go into rollback`);
        curSecondary.reconnect([curPrimary, arbiter]);

        return curPrimary;
    };

    this.stop = function() {
        checkDataConsistency();
        transitionIfAllowed(State.kStopped);
        return rst.stopSet();
    };

    this.getPrimary = function() {
        return curPrimary;
    };

    this.getSecondary = function() {
        return curSecondary;
    };

    this.getTieBreaker = function() {
        return tiebreakerNode;
    };

    this.restartNode = function(nodeId, signal, startOptions, allowedExitCode) {
        assert(signal === SIGKILL || signal === SIGTERM, `Received unknown signal: ${signal}`);
        assert.gte(nodeId, 0, "Invalid argument to RollbackTest.restartNode()");

        const hostName = rst.nodes[nodeId].host;

        if (!TestData.rollbackShutdowns) {
            log(`Not restarting node ${hostName} because 'rollbackShutdowns' was not specified.`);
            return;
        }

        if (nodeId >= kNumDataBearingNodes) {
            log(`Not restarting node ${nodeId} because this replica set is too small.`);
            return;
        }

        if (!TestData.allowUncleanShutdowns && signal !== SIGTERM) {
            log(`Sending node ${hostName} signal ${SIGTERM}` +
                ` instead of ${signal} because 'allowUncleanShutdowns' was not specified.`);
            signal = SIGTERM;
        }

        let opts = {};
        if (allowedExitCode !== undefined) {
            opts = {allowedExitCode: allowedExitCode};
        } else if (signal === SIGKILL) {
            opts = {allowedExitCode: MongoRunner.EXIT_SIGKILL};
        }

        // We may attempt to restart a node while it is in rollback or recovery, in which case
        // the validation checks will fail. We will still validate collections during the
        // RollbackTest's full consistency checks, so we do not lose much validation coverage.
        opts.skipValidation = true;

        log(`Stopping node ${hostName} with signal ${signal}`);
        rst.stop(nodeId, signal, opts, {forRestart: true});
        log(`Restarting node ${hostName}`);
        rst.start(nodeId, startOptions, true /* restart */);

        // Step up if the restarted node is the current primary.
        if (rst.getNodeId(curPrimary) === nodeId) {
            // To prevent below step up from being flaky, we step down and freeze the
            // current secondary to prevent starting a new election. The current secondary
            // can start running election due to explicit step up by the shutting down of current
            // primary if the server parameter "enableElectionHandoff" is set to true.
            rst.freeze(curSecondary);

            const newPrimary = stepUp(curPrimary);
            // As a sanity check, ensure the new primary is the current primary. This is true,
            // because we have configured the replica set with high electionTimeoutMillis.
            assert.eq(newPrimary, curPrimary, "Did not elect the same node as primary");

            // Unfreeze the current secondary so that it can step up again. Retry on network errors
            // in case the current secondary is in ROLLBACK state.
            assert.soon(() => {
                try {
                    assert.commandWorked(curSecondary.adminCommand({replSetFreeze: 0}));
                    return true;
                } catch (e) {
                    if (isNetworkError(e)) {
                        return false;
                    }
                    throw e;
                }
            }, `Failed to unfreeze current secondary ${curSecondary.host}`);
        }

        curSecondary = rst.getSecondary();
        assert.neq(curPrimary, curSecondary);

        waitForState(curSecondary, ReplSetTest.State.SECONDARY);
    };

    /**
     * Returns the underlying ReplSetTest in case the user needs to make adjustments to it.
     */
    this.getTestFixture = function() {
        return rst;
    };

    /**
     * Use this to control the timeout being used in the awaitSecondaryNodesForRollbackTest call
     * in transitionToSteadyStateOperations.
     * For use only in tests that expect unrecoverable rollbacks.
     */
    this.setAwaitSecondaryNodesForRollbackTimeout = function(timeoutMillis) {
        awaitSecondaryNodesForRollbackTimeout = timeoutMillis;
    };
}
