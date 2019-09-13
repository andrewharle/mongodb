// Tests that mongos will wait for CSRS replica set to initiate.

load("jstests/libs/feature_compatibility_version.js");

var configRS = new ReplSetTest({name: "configRS", nodes: 1, useHostName: true});
configRS.startSet({configsvr: '', journal: "", storageEngine: 'wiredTiger'});
var replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
var mongos = MongoRunner.runMongos({configdb: configRS.getURL(), waitForConnect: false});

assert.throws(function() {
    new Mongo(mongos.host);
});

jsTestLog("Initiating CSRS");
configRS.initiate(replConfig);

// Ensure the featureCompatibilityVersion is lastStableFCV so that the mongos can connect if it is
// binary version last-stable.
assert.commandWorked(
    configRS.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

jsTestLog("getting mongos");
var e;
assert.soon(
    function() {
        try {
            mongos2 = new Mongo(mongos.host);
            return true;
        } catch (ex) {
            e = ex;
            return false;
        }
    },
    function() {
        return "mongos " + mongos.host +
            " did not begin accepting connections in time; final exception: " + tojson(e);
    });

jsTestLog("got mongos");
assert.commandWorked(mongos2.getDB('admin').runCommand('serverStatus'));
configRS.stopSet();
MongoRunner.stopMongos(mongos);