/**
 * This test overrides the ShardingTest start/stopBalancer methods to go directly against the config
 * server. The reason is that tests with 3.2 mongos and 3.4 mongod are failing because in that
 * intermediate state neither the new shell is able to stop the balancer (missing balancerStart/Stop
 * commands on mongos), nor the old shell works (because the balancer lock session id does not
 * change since the balancer lock is held permanently).
 */
(function() {
    'use strict';

    // Preserve the original ShardingTest constructor, because we are overriding it
    var originalShardingTest = ShardingTest;

    ShardingTest = function ShardingTestWithMongoSOnLatestVersionForBalancerControl() {
        // The ShardingTest constructor uses the stopBalancer method if the 'enableBalancer' setting
        // is not present. For this reason, temporary patch it to be 'enabled' and then call
        // stopBalancer afterwards.
        if (!arguments[0].other) {
            arguments[0].other = {};
        }

        // Do not start the mongos server for controlling the balancer if the config server is 3.2.
        var skipControlMongoS = false;

        var configOptions = arguments[0].other.configOptions;
        if (configOptions &&
            (configOptions.binVersion == "last-stable" || configOptions.binVersion == "3.2")) {
            skipControlMongoS = true;
        }

        // Check if config server options were specified as an array instead.
        if (arguments[0].config && Array.isArray(arguments[0].config) &&
            (arguments[0].config[0].binVersion == "last-stable" ||
             arguments[0].config[0].binVersion == "3.2")) {
            skipControlMongoS = true;
        }

        if (skipControlMongoS) {
            // Construct the original object and skip creating the separate mongos for controlling
            // the balancer.
            originalShardingTest.apply(this, arguments);
            return;
        }

        var originalEnableBalancer = arguments[0].other.enableBalancer;
        if (!originalEnableBalancer) {
            arguments[0].other.enableBalancer = true;
        }

        // In ShardingTest, enableAutoSplit defaults to the value of enableBalancer. However, this
        // override causes enableBalancer to always be true when constructing ShardingTest. So, if
        // enableAutoSplit is not specified, make sure enableAutoSplit defaults to the original
        // enableBalancer value.
        if (!("enableAutoSplit" in arguments[0].other)) {
            arguments[0].other.enableAutoSplit = originalEnableBalancer;
        }

        // Construct the original object
        originalShardingTest.apply(this, arguments);

        // Start one mongos on the side to be used for balancer control
        var controlMongoSStartupOptions = {configdb: this._configDB, binVersion: 'latest'};

        if (arguments[0].other.keyFile) {
            controlMongoSStartupOptions.keyFile = arguments[0].other.keyFile;
        }

        print('Starting balancer control mongos instance ...');
        var controlMongoSConn = MongoRunner.runMongos(controlMongoSStartupOptions);

        // Override the start/stopBalancer methods

        var originalStartBalancer = this.startBalancer;
        this.startBalancer = function(timeoutMs, interval) {
            timeoutMs = timeoutMs || 60000;

            var result;
            var fn = controlMongoSConn.adminCommand.bind(controlMongoSConn,
                                                         {balancerStart: 1, maxTimeMS: timeoutMs});
            if (controlMongoSStartupOptions.keyFile) {
                result =
                    authutil.asCluster(controlMongoSConn, controlMongoSStartupOptions.keyFile, fn);
            } else {
                result = fn();
            }

            // Back off to the legacy control if the command is not supported
            if (result.code === ErrorCodes.CommandNotFound) {
                return originalStartBalancer.apply(this, [timeoutMs, interval]);
            }

            return assert.commandWorked(result);
        };

        var originalStopBalancer = this.stopBalancer;
        this.stopBalancer = function(timeoutMs, interval) {
            timeoutMs = timeoutMs || 60000;

            var result;
            var fn = controlMongoSConn.adminCommand.bind(controlMongoSConn,
                                                         {balancerStop: 1, maxTimeMS: timeoutMs});
            if (controlMongoSStartupOptions.keyFile) {
                result =
                    authutil.asCluster(controlMongoSConn, controlMongoSStartupOptions.keyFile, fn);
            } else {
                result = fn();
            }

            // Back off to the legacy control if the command is not supported
            if (result.code === ErrorCodes.CommandNotFound) {
                return originalStopBalancer.apply(this, [timeoutMs, interval]);
            }

            return assert.commandWorked(result);
        };

        var originalAwaitBalancerRound = this.awaitBalancerRound;
        this.awaitBalancerRound = function(timeoutMs) {
            timeoutMs = timeoutMs || 60000;

            var fn = originalAwaitBalancerRound.bind(this, timeoutMs, controlMongoSConn);
            if (controlMongoSStartupOptions.keyFile) {
                authutil.asCluster(controlMongoSConn, controlMongoSStartupOptions.keyFile, fn);
            } else {
                fn();
            }
        };

        // Override the stop method to also stop the control mongos
        var originalStop = this.stop;
        this.stop = function() {
            MongoRunner.stopMongos(controlMongoSConn);
            originalStop.apply(this);
        };

        // Start/stop the balancer as requested
        if (!originalEnableBalancer) {
            arguments[0].other.enableBalancer = originalEnableBalancer;
            this.stopBalancer();
        }
    };

    Object.extend(ShardingTest, originalShardingTest);

})();
