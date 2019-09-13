/**
 * Ensures that if the config servers are blackholed from the point of view of MongoS, metadata
 * operations do not get stuck forever.
 *
 * Checking UUID consistency involves talking to config servers through mongos, but mongos is
 * blackholed from the config servers in this test.
 *
 * This test triggers a compiler bug that causes a crash when compiling with optimizations on, see
 * SERVER-35632.
 * @tags: [blacklist_from_rhel_67_s390x]
 */

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    'use strict';

    var st = new ShardingTest({
        shards: 2,
        mongos: 1,
        useBridge: true,
    });

    var testDB = st.s.getDB('BlackHoleDB');

    assert.commandWorked(testDB.adminCommand({enableSharding: 'BlackHoleDB'}));
    assert.commandWorked(
        testDB.adminCommand({shardCollection: testDB.ShardedColl.getFullName(), key: {_id: 1}}));

    assert.writeOK(testDB.ShardedColl.insert({a: 1}));

    jsTest.log('Making all the config servers appear as a blackhole to mongos');
    st._configServers.forEach(function(configSvr) {
        configSvr.discardMessagesFrom(st.s, 1.0);
    });

    assert.commandWorked(testDB.adminCommand({flushRouterConfig: 1}));

    // This shouldn't stall
    jsTest.log('Doing read operation on the sharded collection');
    assert.throws(function() {
        testDB.ShardedColl.find({}).maxTimeMS(15000).itcount();
    });

    // This should fail, because the primary is not available
    jsTest.log('Doing write operation on a new database and collection');
    assert.writeError(st.s.getDB('NonExistentDB')
                          .TestColl.insert({_id: 0, value: 'This value will never be inserted'},
                                           {maxTimeMS: 15000}));

    st.stop();

}());
