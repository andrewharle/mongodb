//
// MultiVersion utility functions for clusters
//

/**
 * Restarts the specified binaries in options with the specified binVersion.
 * Note: this does not perform any upgrade operations.
 *
 * @param binVersion {string}
 * @param options {Object} format:
 *
 * {
 *     upgradeShards: <bool>, // defaults to true
 *     upgradeConfigs: <bool>, // defaults to true
 *     upgradeMongos: <bool>, // defaults to true
 *     waitUntilStable: <bool>, // defaults to false since it provides a more realistic
 *                                 approximation of real-world upgrade behaviour, even though
 *                                 certain tests will likely want a stable cluster after upgrading.
 * }
 */
load("jstests/multiVersion/libs/multi_rs.js");  // Used by upgradeSet.
load("jstests/replsets/rslib.js");              // For awaitRSClientHosts.

ShardingTest.prototype.upgradeCluster = function(binVersion, options) {
    options = options || {};
    if (options.upgradeShards == undefined)
        options.upgradeShards = true;
    if (options.upgradeConfigs == undefined)
        options.upgradeConfigs = true;
    if (options.upgradeMongos == undefined)
        options.upgradeMongos = true;
    if (options.waitUntilStable == undefined)
        options.waitUntilStable = false;

    var upgradedSingleShards = [];

    if (options.upgradeConfigs) {
        // Upgrade config servers if they aren't already upgraded shards
        var numConfigs = this._configServers.length;

        for (var i = 0; i < numConfigs; i++) {
            var configSvr = this._configServers[i];

            if (configSvr.host in upgradedSingleShards) {
                configSvr = upgradedSingleShards[configSvr.host];
            } else {
                MongoRunner.stopMongod(configSvr);
                configSvr = MongoRunner.runMongod(
                    {restart: configSvr, binVersion: binVersion, appendOptions: true});
            }

            this["config" + i] = this["c" + i] = this._configServers[i] = configSvr;
        }
    }

    if (options.upgradeShards) {
        var numShards = this._connections.length;

        // Upgrade shards
        for (var i = 0; i < numShards; i++) {
            if (this._rs && this._rs[i]) {
                // Upgrade replica set
                var rst = this._rs[i].test;
                rst.upgradeSet({binVersion: binVersion});
            } else {
                // Upgrade shard
                var shard = this._connections[i];
                MongoRunner.stopMongod(shard);
                shard = MongoRunner.runMongod(
                    {restart: shard, binVersion: binVersion, appendOptions: true});

                upgradedSingleShards[shard.host] = shard;
                this["shard" + i] = this["d" + i] = this._connections[i] = shard;
            }
        }
    }

    if (options.upgradeMongos) {
        // Upgrade all mongos hosts if specified
        var numMongoses = this._mongos.length;

        for (var i = 0; i < numMongoses; i++) {
            var mongos = this._mongos[i];
            MongoRunner.stopMongos(mongos);

            mongos = MongoRunner.runMongos(
                {restart: mongos, binVersion: binVersion, appendOptions: true});

            this["s" + i] = this._mongos[i] = mongos;
            if (i == 0)
                this.s = mongos;
        }

        this.config = this.s.getDB("config");
        this.admin = this.s.getDB("admin");
    }

    if (options.waitUntilStable) {
        this.waitUntilStable();
    }
};

ShardingTest.prototype.waitUntilStable = function() {
    // Wait for the config server and shards to become available.
    this.configRS.awaitSecondaryNodes();
    let shardPrimaries = [];
    for (let rs of this._rs) {
        rs.test.awaitSecondaryNodes();
        shardPrimaries.push(rs.test.getPrimary());
    }
    // Wait for the ReplicaSetMonitor on mongoS and each shard to reflect the state of all shards.
    for (let client of[...this._mongos, ...shardPrimaries]) {
        awaitRSClientHosts(client, shardPrimaries, {ok: true, ismaster: true});
    }
};

ShardingTest.prototype.restartMongoses = function() {
    var numMongoses = this._mongos.length;

    for (var i = 0; i < numMongoses; i++) {
        var mongos = this._mongos[i];

        MongoRunner.stopMongos(mongos);
        mongos = MongoRunner.runMongos({restart: mongos});

        this["s" + i] = this._mongos[i] = mongos;
        if (i == 0)
            this.s = mongos;
    }

    this.config = this.s.getDB("config");
    this.admin = this.s.getDB("admin");
};

ShardingTest.prototype.getMongosAtVersion = function(binVersion) {
    var mongoses = this._mongos;
    for (var i = 0; i < mongoses.length; i++) {
        try {
            var version = mongoses[i].getDB("admin").runCommand("serverStatus").version;
            if (version.indexOf(binVersion) == 0) {
                return mongoses[i];
            }
        } catch (e) {
            printjson(e);
            print(mongoses[i]);
        }
    }
};
