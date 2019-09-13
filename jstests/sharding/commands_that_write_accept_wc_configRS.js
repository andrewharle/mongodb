/**
 * This file tests that commands that do writes accept a write concern in a sharded cluster. This
 * test defines various database commands and what they expect to be true before and after the fact.
 * It then runs the commands with various invalid writeConcerns and valid writeConcerns and
 * ensures that they succeed and fail appropriately. For the valid writeConcerns, the test stops
 * replication between nodes to make sure the write concern is actually being waited for. This only
 * tests commands that get sent to config servers and must have w: majority specified. If these
 * commands fail, they should return an actual error, not just a writeConcernError.
 *
 * This test is labeled resource intensive because its total io_write is 70MB compared to a median
 * of 5MB across all sharding tests in wiredTiger. Its total io_write is 1900MB compared to a median
 * of 135MB in mmapv1.
 * @tags: [resource_intensive]
 */
load('jstests/libs/write_concern_util.js');
load('jstests/multiVersion/libs/auth_helpers.js');

(function() {
    "use strict";

    // TODO SERVER-35447: Multiple users cannot be authenticated on one connection within a session.
    TestData.disableImplicitSessions = true;

    var st = new ShardingTest({
        // Set priority of secondaries to zero to prevent spurious elections.
        shards: {
            rs0: {
                nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
                settings: {chainingAllowed: false}
            },
            rs1: {
                nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
                settings: {chainingAllowed: false}
            }
        },
        configReplSetTestOptions: {settings: {chainingAllowed: false}},
        mongos: 1
    });

    var mongos = st.s;
    var dbName = "wc-test-configRS";
    var db = mongos.getDB(dbName);
    var adminDB = mongos.getDB('admin');
    // A database connection on a local shard, rather than through the mongos.
    var localDB = st.shard0.getDB('localWCTest');
    var collName = 'leaves';
    var coll = db[collName];
    var counter = 0;

    function dropTestData() {
        st.configRS.awaitReplication();
        st.rs0.awaitReplication();
        st.rs1.awaitReplication();
        db.dropUser('username');
        db.dropUser('user1');
        localDB.dropUser('user2');
        assert(!db.auth("username", "password"), "auth should have failed");
        getNewDB();
    }

    // We get new databases because we do not want to reuse dropped databases that may be in a
    // bad state. This test calls dropDatabase when config server secondary nodes are down, so the
    // command fails after only the database metadata is dropped from the config servers, but the
    // data on the shards still remains. This makes future operations, such as moveChunk, fail.
    function getNewDB() {
        db = mongos.getDB(dbName + counter);
        counter++;
        coll = db[collName];
    }

    // Commands in 'commands' will accept any valid writeConcern.
    var commands = [];

    commands.push({
        req: {createUser: 'username', pwd: 'password', roles: jsTest.basicUserRoles},
        setupFunc: function() {},
        confirmFunc: function() {
            assert(db.auth("username", "password"), "auth failed");
        },
        requiresMajority: true,
        runsOnShards: false,
        failsOnShards: false,
        admin: false
    });

    commands.push({
        req: {updateUser: 'username', pwd: 'password2', roles: jsTest.basicUserRoles},
        setupFunc: function() {
            db.runCommand({createUser: 'username', pwd: 'password', roles: jsTest.basicUserRoles});
        },
        confirmFunc: function() {
            assert(!db.auth("username", "password"), "auth should have failed");
            assert(db.auth("username", "password2"), "auth failed");
        },
        requiresMajority: true,
        runsOnShards: false,
        admin: false
    });

    commands.push({
        req: {dropUser: 'tempUser'},
        setupFunc: function() {
            db.runCommand({createUser: 'tempUser', pwd: 'password', roles: jsTest.basicUserRoles});
            assert(db.auth("tempUser", "password"), "auth failed");
        },
        confirmFunc: function() {
            assert(!db.auth("tempUser", "password"), "auth should have failed");
        },
        requiresMajority: true,
        runsOnShards: false,
        failsOnShards: false,
        admin: false
    });

    function testInvalidWriteConcern(wc, cmd) {
        if (wc.w === 2 && !cmd.requiresMajority) {
            return;
        }
        cmd.req.writeConcern = wc;
        jsTest.log("Testing " + tojson(cmd.req));

        dropTestData();
        cmd.setupFunc();
        var res = runCommandCheckAdmin(db, cmd);
        assert.commandFailed(res);
        assert(!res.writeConcernError,
               'bad writeConcern on config server had writeConcernError. ' +
                   tojson(res.writeConcernError));
    }

    function runCommandFailOnShardsPassOnConfigs(cmd) {
        var req = cmd.req;
        var res;
        // This command is run on the shards in addition to the config servers.
        if (cmd.runsOnShards) {
            if (cmd.failsOnShards) {
                // This command fails when there is a writeConcernError on the shards.
                // We set the timeout high enough that the command should not time out against the
                // config server, but not exorbitantly high, because it will always time out against
                // shards and so will increase the runtime of this test.
                req.writeConcern.wtimeout = 15 * 1000;
                res = runCommandCheckAdmin(db, cmd);
                restartReplicationOnAllShards(st);
                assert.commandFailed(res);
                assert(
                    !res.writeConcernError,
                    'command on config servers with a paused replicaset had writeConcernError: ' +
                        tojson(res));
            } else {
                // This command passes and returns a writeConcernError when there is a
                // writeConcernError on the shards.
                // We set the timeout high enough that the command should not time out against the
                // config server, but not exorbitantly high, because it will always time out against
                // shards and so will increase the runtime of this test.
                req.writeConcern.wtimeout = 15 * 1000;
                res = runCommandCheckAdmin(db, cmd);
                restartReplicationOnAllShards(st);
                assert.commandWorked(res);
                cmd.confirmFunc();
                assertWriteConcernError(res);
            }
        } else {
            // This command is only run on the config servers and so should pass when shards are
            // not replicating.
            res = runCommandCheckAdmin(db, cmd);
            restartReplicationOnAllShards(st);
            assert.commandWorked(res);
            cmd.confirmFunc();
            assert(!res.writeConcernError,
                   'command on config servers with a paused replicaset had writeConcernError: ' +
                       tojson(res));
        }
    }

    function testValidWriteConcern(wc, cmd) {
        var req = cmd.req;
        var setupFunc = cmd.setupFunc;
        var confirmFunc = cmd.confirmFunc;

        req.writeConcern = wc;
        jsTest.log("Testing " + tojson(req));

        dropTestData();
        setupFunc();

        // Command with a full cluster should succeed.
        var res = runCommandCheckAdmin(db, cmd);
        assert.commandWorked(res);
        assert(!res.writeConcernError,
               'command on a full cluster had writeConcernError: ' + tojson(res));
        confirmFunc();

        dropTestData();
        setupFunc();
        // Stop replication at all shard secondaries.
        stopReplicationOnSecondariesOfAllShards(st);

        // Command is running on full config server replica set but a majority of a shard's
        // nodes are down.
        runCommandFailOnShardsPassOnConfigs(cmd);

        dropTestData();
        setupFunc();
        // Stop replication at all config server secondaries and all shard secondaries.
        stopReplicationOnSecondariesOfAllShards(st);
        st.configRS.awaitReplication();
        stopReplicationOnSecondaries(st.configRS);

        // Command should fail after two config servers are not replicating.
        req.writeConcern.wtimeout = 3000;
        res = runCommandCheckAdmin(db, cmd);
        restartReplicationOnAllShards(st);
        assert.commandFailed(res);
        assert(!res.writeConcernError,
               'command on config servers with a paused replicaset had writeConcernError: ' +
                   tojson(res));
    }

    var majorityWC = {w: 'majority', wtimeout: ReplSetTest.kDefaultTimeoutMS};

    // Config server commands require w: majority writeConcerns.
    var nonMajorityWCs = [{w: 'invalid'}, {w: 2}];

    commands.forEach(function(cmd) {
        nonMajorityWCs.forEach(function(wc) {
            testInvalidWriteConcern(wc, cmd);
        });
        testValidWriteConcern(majorityWC, cmd);
    });

    st.stop();
})();
