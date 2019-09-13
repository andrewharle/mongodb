/**
 * Checks that:
 * 1) Issuing a metadata command through a mongos with any write concern succeeds (because we
 * convert it up to majority WC),
 * 2) Issuing a metadata command directly to a config server with non-majority write concern fails.
 */
(function() {
    'use strict';

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;
    const newShardName = "newShard";

    // Commands sent directly to the config server should fail with WC < majority.
    const unacceptableWCsForConfig = [
        {writeConcern: {w: 0}},
        {writeConcern: {w: 1}},
        {writeConcern: {w: 2}},
        {writeConcern: {w: 3}},
        // TODO: should metadata commands allow j: false? can CSRS have an in-memory storage engine?
        // writeConcern{w: "majority", j: "false"}},
    ];

    // Unspecified WC and WC majority should succeed.
    const acceptableWCsForConfig = [
        {},
        {writeConcern: {w: "majority"}},
        {writeConcern: {w: "majority", wtimeout: 15000}},
    ];

    // Any write concern can be sent to a mongos, because mongos will upconvert it to majority.
    const unacceptableWCsForMongos = [];
    const acceptableWCsForMongos = [
        {},
        {writeConcern: {w: 0}},
        {writeConcern: {w: 0, wtimeout: 15000}},
        {writeConcern: {w: 1}},
        {writeConcern: {w: 2}},
        {writeConcern: {w: 3}},
        {writeConcern: {w: "majority"}},
        {writeConcern: {w: "majority", wtimeout: 15000}},
    ];

    const setupFuncs = {
        noop: function() {},
        createDatabase: function() {
            // A database is implicitly created when a collection within it is created.
            assert.commandWorked(st.s.getDB(dbName).runCommand({create: collName}));
        },
        enableSharding: function() {
            assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
        },
        addShard: function() {
            assert.commandWorked(st.s.adminCommand({addShard: newShard.name, name: newShardName}));
        },
    };

    const cleanupFuncs = {
        noop: function() {},
        dropDatabase: function() {
            assert.commandWorked(st.s.getDB(dbName).runCommand({dropDatabase: 1}));
        },
        removeShardIfExists: function() {
            var res = st.s.adminCommand({removeShard: newShardName});
            if (!res.ok && res.code == ErrorCodes.ShardNotFound) {
                return;
            }
            assert.commandWorked(res);
            assert.eq('started', res.state);
            res = st.s.adminCommand({removeShard: newShardName});
            assert.commandWorked(res);
            assert.eq('completed', res.state);
        },
    };

    function checkCommand(
        conn, command, unacceptableWCs, acceptableWCs, adminCommand, setupFunc, cleanupFunc) {
        unacceptableWCs.forEach(function(writeConcern) {
            jsTest.log("testing " + tojson(command) + " with writeConcern " + tojson(writeConcern) +
                       " against " + conn + ", expecting the command to fail");
            setupFunc();
            let commandWithWriteConcern = {};
            Object.assign(commandWithWriteConcern, command, writeConcern);
            if (adminCommand) {
                assert.commandFailedWithCode(conn.adminCommand(commandWithWriteConcern),
                                             ErrorCodes.InvalidOptions);
            } else {
                assert.commandFailedWithCode(conn.runCommand(commandWithWriteConcern),
                                             ErrorCodes.InvalidOptions);
            }
            cleanupFunc();
        });

        acceptableWCs.forEach(function(writeConcern) {
            jsTest.log("testing " + tojson(command) + " with writeConcern " + tojson(writeConcern) +
                       " against " + conn + ", expecting the command to succeed");
            setupFunc();
            let commandWithWriteConcern = {};
            Object.assign(commandWithWriteConcern, command, writeConcern);
            if (adminCommand) {
                assert.commandWorked(conn.adminCommand(commandWithWriteConcern));
            } else {
                assert.commandWorked(conn.runCommand(commandWithWriteConcern));
            }
            cleanupFunc();
        });
    }

    function checkCommandMongos(command, setupFunc, cleanupFunc) {
        checkCommand(st.s,
                     command,
                     unacceptableWCsForMongos,
                     acceptableWCsForMongos,
                     true,
                     setupFunc,
                     cleanupFunc);
    }

    function checkCommandConfigSvr(command, setupFunc, cleanupFunc) {
        checkCommand(st.configRS.getPrimary(),
                     command,
                     unacceptableWCsForConfig,
                     acceptableWCsForConfig,
                     true,
                     setupFunc,
                     cleanupFunc);
    }

    var st = new ShardingTest({shards: 1});

    // enableSharding
    checkCommandMongos({enableSharding: dbName}, setupFuncs.noop, cleanupFuncs.dropDatabase);
    checkCommandConfigSvr(
        {_configsvrEnableSharding: dbName}, setupFuncs.noop, cleanupFuncs.dropDatabase);

    // movePrimary
    checkCommandMongos({movePrimary: dbName, to: st.shard0.name},
                       setupFuncs.createDatabase,
                       cleanupFuncs.dropDatabase);
    checkCommandConfigSvr({_configsvrMovePrimary: dbName, to: st.shard0.name},
                          setupFuncs.createDatabase,
                          cleanupFuncs.dropDatabase);

    // We are using a different name from ns because it was already created in setupFuncs.
    checkCommandConfigSvr({_configsvrCreateCollection: dbName + '.bar', options: {}},
                          setupFuncs.createDatabase,
                          cleanupFuncs.dropDatabase);

    // shardCollection
    checkCommandMongos(
        {shardCollection: ns, key: {_id: 1}}, setupFuncs.enableSharding, cleanupFuncs.dropDatabase);
    checkCommandConfigSvr({_configsvrShardCollection: ns, key: {_id: 1}},
                          setupFuncs.enableSharding,
                          cleanupFuncs.dropDatabase);

    // createDatabase
    // Don't check createDatabase against mongos: there is no createDatabase command exposed on
    // mongos; a database is created implicitly when a collection in it is created.
    checkCommandConfigSvr({_configsvrCreateDatabase: dbName, to: st.shard0.name},
                          setupFuncs.noop,
                          cleanupFuncs.dropDatabase);

    // addShard
    var newShard = MongoRunner.runMongod({shardsvr: ""});
    checkCommandMongos({addShard: newShard.name, name: newShardName},
                       setupFuncs.noop,
                       cleanupFuncs.removeShardIfExists);
    checkCommandConfigSvr({_configsvrAddShard: newShard.name, name: newShardName},
                          setupFuncs.noop,
                          cleanupFuncs.removeShardIfExists);

    // removeShard
    checkCommandMongos({removeShard: newShardName}, setupFuncs.addShard, cleanupFuncs.noop);
    checkCommandConfigSvr(
        {_configsvrRemoveShard: newShardName}, setupFuncs.addShard, cleanupFuncs.noop);

    // dropCollection
    checkCommandMongos({drop: ns}, setupFuncs.createDatabase, cleanupFuncs.dropDatabase);
    checkCommandConfigSvr(
        {_configsvrDropCollection: ns}, setupFuncs.createDatabase, cleanupFuncs.dropDatabase);

    // dropDatabase

    // We can't use the checkCommandMongos wrapper because we need a connection to the test
    // database.
    checkCommand(st.s.getDB(dbName),
                 {dropDatabase: 1},
                 unacceptableWCsForMongos,
                 acceptableWCsForMongos,
                 false,
                 setupFuncs.createDatabase,
                 cleanupFuncs.dropDatabase);
    checkCommandConfigSvr(
        {_configsvrDropDatabase: dbName}, setupFuncs.createDatabase, cleanupFuncs.dropDatabase);

    MongoRunner.stopMongos(newShard);
    st.stop();
})();
