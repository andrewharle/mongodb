/**
 * Test that removing and re-adding shard works correctly.
 *
 * This test is labeled resource intensive because its total io_write is 59MB compared to a median
 * of 5MB across all sharding tests in wiredTiger. Its total io_write is 918MB compared to a median
 * of 135MB in mmapv1.
 * @tags: [resource_intensive]
 */
load("jstests/replsets/rslib.js");

// The UUID consistency check uses connections to shards cached on the ShardingTest object, but this
// test restarts a shard, so the cached connection is not usable.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    'use strict';

    function seedString(replTest) {
        var members = replTest.getReplSetConfig().members.map(function(elem) {
            return elem.host;
        });
        return replTest.name + '/' + members.join(',');
    }

    function removeShard(st, replTest) {
        jsTest.log("Removing shard with name: " + replTest.name);
        var res = st.s.adminCommand({removeShard: replTest.name});
        assert.commandWorked(res);
        assert.eq('started', res.state);
        assert.soon(function() {
            res = st.s.adminCommand({removeShard: replTest.name});
            assert.commandWorked(res);
            return ('completed' === res.state);
        }, "failed to remove shard: " + tojson(res));

        // Drop the database so the shard can be re-added.
        assert.commandWorked(replTest.getPrimary().getDB(coll.getDB().getName()).dropDatabase());
    }

    function addShard(st, replTest) {
        var seed = seedString(replTest);
        print("Adding shard with seed: " + seed);
        try {
            assert.eq(true, st.adminCommand({addshard: seed}));
        } catch (e) {
            print("First attempt to addShard failed, trying again");
            // transport error on first attempt is expected.  Make sure second attempt goes through
            assert.eq(true, st.adminCommand({addshard: seed}));
        }
        awaitRSClientHosts(
            new Mongo(st.s.host), replTest.getSecondaries(), {ok: true, secondary: true});

        assert.soon(function() {
            var x = st.chunkDiff(coll.getName(), coll.getDB().getName());
            print("chunk diff: " + x);
            return x < 2;
        }, "no balance happened", 30 * 60 * 1000);

        try {
            assert.eq(300, coll.find().itcount());
        } catch (e) {
            // Expected.  First query might get transport error and need to reconnect.
            printjson(e);
            assert.eq(300, coll.find().itcount());
        }
        print("Shard added successfully");
    }

    var st = new ShardingTest(
        {shards: {rs0: {nodes: 2}, rs1: {nodes: 2}}, other: {chunkSize: 1, enableBalancer: true}});

    // Pending resolution of SERVER-8598, we need to wait for deletion after chunk migrations to
    // avoid a pending delete re-creating a database after it was dropped.
    st.s.getDB("config").settings.update({_id: "balancer"}, {$set: {_waitForDelete: true}}, true);

    var conn = new Mongo(st.s.host);
    var coll = conn.getCollection("test.remove2");
    coll.drop();

    assert.commandWorked(st.s0.adminCommand({enableSharding: coll.getDB().getName()}));
    st.ensurePrimaryShard(coll.getDB().getName(), st.shard0.shardName);
    assert.commandWorked(st.s0.adminCommand({shardCollection: coll.getFullName(), key: {i: 1}}));

    // Setup initial data
    var str = 'a';
    while (str.length < 1024 * 16) {
        str += str;
    }

    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < 300; i++) {
        bulk.insert({i: i % 10, str: str});
    }
    assert.writeOK(bulk.execute());

    assert.eq(300, coll.find().itcount());

    assert.soon(function() {
        var x = st.chunkDiff('remove2', "test");
        print("chunk diff: " + x);
        return x < 2;
    }, "no balance happened", 30 * 60 * 1000);

    assert.eq(300, coll.find().itcount());

    st.printShardingStatus();

    var rst1 = st.rs1;
    // Remove shard and add it back in, without shutting it down.
    jsTestLog("Attempting to remove shard and add it back in");
    removeShard(st, rst1);
    addShard(st, rst1);

    // Remove shard, restart set, then add it back in.
    jsTestLog("Attempting to remove shard, restart the set, and then add it back in");
    var originalSeed = seedString(rst1);

    removeShard(st, rst1);
    rst1.stopSet();
    print("Sleeping for 20 seconds to let the other shard's ReplicaSetMonitor time out");
    sleep(20000);  // 1 failed check should take 10 seconds, sleep for 20 just to be safe

    rst1.startSet({restart: true});
    rst1.initiate();
    rst1.awaitReplication();

    assert.eq(
        originalSeed, seedString(rst1), "Set didn't come back up with the same hosts as before");
    addShard(st, rst1);

    // Shut down shard and wait for its ReplicaSetMonitor to be cleaned up, then start it back up
    // and use it.
    //
    // TODO: test this both with AND without waiting for the ReplicaSetMonitor to be cleaned up.
    //
    // This part doesn't pass, even without cleaning up the ReplicaSetMonitor - see SERVER-5900.
    /*
    printjson( conn.getDB('admin').runCommand({movePrimary : 'test2', to : rst1.name}) );
    printjson( conn.getDB('admin').runCommand({setParameter : 1, replMonitorMaxFailedChecks : 5}) );
    jsTestLog( "Shutting down set" )
    rst1.stopSet();
    jsTestLog( "sleeping for 20 seconds to make sure ReplicaSetMonitor gets cleaned up");
    sleep(20000); // 1 failed check should take 10 seconds, sleep for 20 just to be safe

    // Should fail since rst1 is the primary for test2
    assert.throws(function() {conn.getDB('test2').foo.find().itcount()});
    jsTestLog( "Bringing set back up" );
    rst1.startSet();
    rst1.initiate();
    rst1.awaitReplication();

    jsTestLog( "Checking that set is usable again" );
    //conn.getDB('admin').runCommand({flushRouterConfig:1}); // Uncommenting this makes test pass
    conn.getDB('test2').foo.insert({a:1});
    gle = conn.getDB('test2').runCommand('getLastError');
    if ( !gle.ok ) {
        // Expected.  First write will fail and need to re-connect
        print( "write failed" );
        printjson( gle );
        conn.getDB('test2').foo.insert({a:1});
        assert( conn.getDB('test2').getLastErrorObj().ok );
    }

    assert.eq( 1, conn.getDB('test2').foo.find().itcount() );
    assert( conn.getDB('test2').dropDatabase().ok );
    */

    // Remove shard and add a new shard with the same replica set and shard name, but different
    // ports
    jsTestLog("Attempt removing shard and adding a new shard with the same Replica Set name");
    removeShard(st, rst1);
    rst1.stopSet();
    print("Sleeping for 60 seconds to let the other shards restart their ReplicaSetMonitors");
    sleep(60000);

    var rst2 = new ReplSetTest({name: rst1.name, nodes: 2, useHostName: true});
    rst2.startSet({shardsvr: ""});
    rst2.initiate();
    rst2.awaitReplication();

    addShard(st, rst2);
    printjson(st.admin.runCommand({movePrimary: 'test2', to: rst2.name}));

    assert.eq(300, coll.find().itcount());
    conn.getDB('test2').foo.insert({a: 1});
    assert.eq(1, conn.getDB('test2').foo.find().itcount());

    // Can't shut down with rst2 in the set or ShardingTest will fail trying to cleanup on shutdown.
    // Have to take out rst2 and put rst1 back into the set so that it can clean up.
    jsTestLog("Putting ShardingTest back to state it expects");
    printjson(st.admin.runCommand({movePrimary: 'test2', to: st.rs0.name}));
    removeShard(st, rst2);
    rst2.stopSet();

    print("Sleeping for 60 seconds to let the other shards restart their ReplicaSetMonitors");
    sleep(60000);

    rst1.startSet({restart: true});
    rst1.initiate();
    rst1.awaitReplication();

    assert.eq(
        originalSeed, seedString(rst1), "Set didn't come back up with the same hosts as before");
    addShard(st, rst1);

    st.stop();
})();
