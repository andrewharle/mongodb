/**
 * Loading this file overrides DB.prototype.getCollection() with a function that attempts to shard
 * the collection before returning it.
 *
 * The DB.prototype.getCollection() function is called whenever an undefined property is accessed
 * on the db object.
 *
 * DBCollection.prototype.drop() function will re-shard any non-blacklisted collection that is
 * dropped in a sharded cluster.
 */

(function() {
    'use strict';

    load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.

    // Save a reference to the original methods in the IIFE's scope.
    // This scoping allows the original methods to be called by the overrides below.
    var originalGetCollection = DB.prototype.getCollection;
    var originalDBCollectionDrop = DBCollection.prototype.drop;
    var originalStartParallelShell = startParallelShell;
    var originalRunCommand = Mongo.prototype.runCommand;

    var testMayRunDropInParallel = false;

    // Blacklisted namespaces that should not be sharded.
    var blacklistedNamespaces = [
        /\$cmd/,
        /^admin\./,
        /^config\./,
        /\.system\./,
    ];

    function shardCollection(collection) {
        var db = collection.getDB();
        var dbName = db.getName();
        var fullName = collection.getFullName();

        for (var ns of blacklistedNamespaces) {
            if (fullName.match(ns)) {
                return;
            }
        }

        var res = db.adminCommand({enableSharding: dbName});

        // enableSharding may only be called once for a database.
        if (res.code !== ErrorCodes.AlreadyInitialized) {
            assert.commandWorked(res, "enabling sharding on the '" + dbName + "' db failed");
        }

        res = db.adminCommand(
            {shardCollection: fullName, key: {_id: 'hashed'}, collation: {locale: "simple"}});
        if (res.ok === 0 && testMayRunDropInParallel) {
            // We ignore ConflictingOperationInProgress error responses from the
            // "shardCollection" command if it's possible the test was running a "drop" command
            // concurrently. We could retry running the "shardCollection" command, but tests
            // that are likely to trigger this case are also likely running the "drop" command
            // in a loop. We therefore just let the test continue with the collection being
            // unsharded.
            assert.commandFailedWithCode(res, ErrorCodes.ConflictingOperationInProgress);
            print("collection '" + fullName +
                  "' failed to be sharded due to a concurrent drop operation");
        } else {
            assert.commandWorked(res, "sharding '" + fullName + "' with a hashed _id key failed");
        }
    }

    DB.prototype.getCollection = function() {
        var collection = originalGetCollection.apply(this, arguments);

        // The following "collStats" command can behave unexpectedly when running in a causal
        // consistency suite with secondary read preference. "collStats" does not support causal
        // consistency, making it possible to see a stale view of the collection if run on a
        // secondary, potentially causing shardCollection() to be called when it shouldn't.
        // E.g. if the collection has just been sharded but not yet visible on the
        // secondary, we could end up calling shardCollection on it again, which would fail.
        //
        // The workaround is to use a TestData flag to temporarily bypass the read preference
        // override.
        const testDataDoNotOverrideReadPreferenceOriginal = TestData.doNotOverrideReadPreference;
        let collStats;

        try {
            TestData.doNotOverrideReadPreference = true;
            collStats = this.runCommand({collStats: collection.getName()});
        } finally {
            TestData.doNotOverrideReadPreference = testDataDoNotOverrideReadPreferenceOriginal;
        }

        // If the collection is already sharded or is non-empty, do not attempt to shard.
        if (collStats.sharded || collStats.count > 0) {
            return collection;
        }

        // Attempt to enable sharding on database and collection if not already done.
        shardCollection(collection);

        return collection;
    };

    DBCollection.prototype.drop = function() {
        var dropResult = originalDBCollectionDrop.apply(this, arguments);

        // Attempt to enable sharding on database and collection if not already done.
        shardCollection(this);

        return dropResult;
    };

    // The mapReduce command has a special requirement where the command must indicate the output
    // collection is sharded, so we must be sure to add this information in this passthrough.
    Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
        // Skip any commands that are not mapReduce or do not have an 'out' option.
        if (typeof cmdObj !== 'object' || cmdObj === null ||
            (!cmdObj.hasOwnProperty('mapreduce') && !cmdObj.hasOwnProperty('mapReduce')) ||
            !cmdObj.hasOwnProperty('out')) {
            return originalRunCommand.apply(this, arguments);
        }

        const originalCmdObj = Object.merge({}, cmdObj);

        // SERVER-5448 'jsMode' is not supported through mongos. The 'jsMode' should not impact the
        // results at all, so can be safely deleted in the sharded environment.
        delete cmdObj.jsMode;

        // Modify the output options to specify that the collection is sharded.
        let outputSpec = cmdObj.out;
        if (typeof(outputSpec) === "string") {
            this.getDB(dbName)[outputSpec].drop();  // This will implicitly shard it.
            outputSpec = {replace: outputSpec, sharded: true};
        } else if (typeof(outputSpec) !== "object") {
            // This is a malformed command, just send it along.
            return originalRunCommand.apply(this, arguments);
        } else if (!outputSpec.hasOwnProperty("sharded")) {
            let outputColl = null;
            if (outputSpec.hasOwnProperty("replace")) {
                outputColl = outputSpec.replace;
            } else if (outputSpec.hasOwnProperty("merge")) {
                outputColl = outputSpec.merge;
            } else if (outputSpec.hasOwnProperty("reduce")) {
                outputColl = outputSpec.reduce;
            }

            if (outputColl === null) {
                // This is a malformed command, just send it along.
                return originalRunCommand.apply(this, arguments);
            }
            this.getDB(dbName)[outputColl].drop();  // This will implicitly shard it.
            outputSpec.sharded = true;
        }

        cmdObj.out = outputSpec;
        jsTestLog('Overriding mapReduce command. Original command: ' + tojson(originalCmdObj) +
                  ' New command: ' + tojson(cmdObj));
        return originalRunCommand.apply(this, arguments);
    };

    // Tests may use a parallel shell to run the "drop" command concurrently with other
    // operations. This can cause the "shardCollection" command to return a
    // ConflictingOperationInProgress error response.
    startParallelShell = function() {
        testMayRunDropInParallel = true;
        return originalStartParallelShell.apply(this, arguments);
    };

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/implicitly_shard_accessed_collections.js");

}());
