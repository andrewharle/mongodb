/**
 * Tests that the addShard process initializes sharding awareness on an added standalone or
 * replica set shard that was started with --shardsvr.
 */

(function() {
    "use strict";

    var waitForIsMaster = function(conn) {
        assert.soon(function() {
            var res = conn.getDB('admin').runCommand({isMaster: 1});
            return res.ismaster;
        });
    };

    var checkShardingStateInitialized = function(conn, configConnStr, shardName, clusterId) {
        var res = conn.getDB('admin').runCommand({shardingState: 1});
        assert.commandWorked(res);
        assert(res.enabled);
        assert.eq(configConnStr, res.configServer);
        assert.eq(shardName, res.shardName);
        assert(clusterId.equals(res.clusterId),
               'cluster id: ' + tojson(clusterId) + ' != ' + tojson(res.clusterId));
    };

    var checkShardMarkedAsShardAware = function(mongosConn, shardName) {
        var res = mongosConn.getDB('config').getCollection('shards').findOne({_id: shardName});
        assert.neq(null, res, "Could not find new shard " + shardName + " in config.shards");
        assert.eq(1, res.state);
    };

    // Create the cluster to test adding shards to.
    var st = new ShardingTest({shards: 1});
    var clusterId = st.s.getDB('config').getCollection('version').findOne().clusterId;

    // Add a shard that is a standalone mongod.

    var standaloneConn = MongoRunner.runMongod({shardsvr: ''});
    waitForIsMaster(standaloneConn);

    jsTest.log("Going to add standalone as shard: " + standaloneConn);
    var newShardName = "newShard";
    assert.commandWorked(st.s.adminCommand({addShard: standaloneConn.name, name: newShardName}));
    checkShardingStateInitialized(standaloneConn, st.configRS.getURL(), newShardName, clusterId);
    checkShardMarkedAsShardAware(st.s, newShardName);

    MongoRunner.stopMongod(standaloneConn);

    // Add a shard that is a replica set.

    var replTest = new ReplSetTest({nodes: 1});
    replTest.startSet({shardsvr: ''});
    replTest.initiate();
    waitForIsMaster(replTest.getPrimary());

    jsTest.log("Going to add replica set as shard: " + tojson(replTest));
    assert.commandWorked(st.s.adminCommand({addShard: replTest.getURL(), name: replTest.getURL()}));
    checkShardingStateInitialized(
        replTest.getPrimary(), st.configRS.getURL(), replTest.getURL(), clusterId);
    checkShardMarkedAsShardAware(st.s, newShardName);

    replTest.stopSet();

    st.stop();

})();
