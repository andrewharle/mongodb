/**
 * Get the URI of the wt collection file given the collection name.
 */
let getUriForColl = function(coll) {
    assert(coll.exists());  // Collection must exist
    return coll.stats().wiredTiger.uri.split("table:")[1];
};

/**
 * Get the URI of the wt index file given the collection name and the index name.
 */
let getUriForIndex = function(coll, indexName) {
    assert(coll.exists());  // Collection must exist
    const ret = assert.commandWorked(coll.getDB().runCommand({collStats: coll.getName()}));
    return ret.indexDetails[indexName].uri.split("table:")[1];
};

/**
 * 'Corrupt' the file by replacing it with an empty file.
 */
let corruptFile = function(file) {
    removeFile(file);
    writeFile(file, "");
};

/**
 * Starts a mongod on the provided data path without clearing data. Accepts 'options' as parameters
 * to runMongod.
 */
let startMongodOnExistingPath = function(dbpath, options) {
    let args = {dbpath: dbpath, noCleanData: true};
    for (let attr in options) {
        if (options.hasOwnProperty(attr))
            args[attr] = options[attr];
    }
    return MongoRunner.runMongod(args);
};

let assertQueryUsesIndex = function(coll, query, indexName) {
    let res = coll.find(query).explain();
    assert.commandWorked(res);

    let inputStage = res.queryPlanner.winningPlan.inputStage;
    assert.eq(inputStage.stage, "IXSCAN");
    assert.eq(inputStage.indexName, indexName);
};

/**
 * Assert that running MongoDB with --repair on the provided dbpath exits cleanly.
 */
let assertRepairSucceeds = function(dbpath, port, opts) {
    let args = ["mongod", "--repair", "--port", port, "--dbpath", dbpath, "--bind_ip_all"];
    for (let a in opts) {
        if (opts.hasOwnProperty(a))
            args.push("--" + a);

        if (opts[a].length > 0) {
            args.push(a);
        }
    }
    jsTestLog("Repairing the node");
    assert.eq(0, runMongoProgram.apply(this, args));
};

let assertRepairFailsWithFailpoint = function(dbpath, port, failpoint) {
    const param = "failpoint." + failpoint + "={'mode': 'alwaysOn'}";
    jsTestLog("The node should fail to complete repair with --setParameter " + param);

    assert.eq(
        MongoRunner.EXIT_ABRUPT,
        runMongoProgram(
            "mongod", "--repair", "--port", port, "--dbpath", dbpath, "--setParameter", param));
};

/**
 * Assert that starting MongoDB with --replSet on an existing data path exits with a specific
 * error.
 */
let assertErrorOnStartupWhenStartingAsReplSet = function(dbpath, port, rsName) {
    jsTestLog("The repaired node should fail to start up with the --replSet option");

    clearRawMongoProgramOutput();
    let node = MongoRunner.runMongod(
        {dbpath: dbpath, port: port, replSet: rsName, noCleanData: true, waitForConnect: false});
    assert.soon(function() {
        return rawMongoProgramOutput().indexOf("Fatal Assertion 50923") >= 0;
    });
    MongoRunner.stopMongod(node, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
};

/**
 * Assert that starting MongoDB as a standalone on an existing data path exits with a specific
 * error because the previous repair failed.
 */
let assertErrorOnStartupAfterIncompleteRepair = function(dbpath, port) {
    jsTestLog("The node should fail to start up because a previous repair did not complete");

    clearRawMongoProgramOutput();
    let node = MongoRunner.runMongod(
        {dbpath: dbpath, port: port, noCleanData: true, waitForConnect: false});
    assert.soon(function() {
        return rawMongoProgramOutput().indexOf("Fatal Assertion 50922") >= 0;
    });
    MongoRunner.stopMongod(node, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
};

/**
 * Assert that starting MongoDB as a standalone on an existing data path succeeds. Uses a provided
 * testFunc to run any caller-provided checks on the started node.
 */
let assertStartAndStopStandaloneOnExistingDbpath = function(dbpath, port, testFunc) {
    jsTestLog("The repaired node should start up and serve reads as a standalone");
    let node = MongoRunner.runMongod({dbpath: dbpath, port: port, noCleanData: true});
    assert(node);
    testFunc(node);
    MongoRunner.stopMongod(node);
};

/**
 * Assert that starting MongoDB with --replSet succeeds. Uses a provided testFunc to run any
 * caller-provided checks on the started node.
 *
 * Returns the started node.
 */
let assertStartInReplSet = function(replSet, originalNode, cleanData, expectResync, testFunc) {
    jsTestLog("The node should rejoin the replica set. Clean data: " + cleanData,
              ". Expect resync: " + expectResync);
    let node = replSet.start(
        originalNode, {dbpath: originalNode.dbpath, port: originalNode.port, restart: !cleanData});

    replSet.awaitSecondaryNodes();

    // Ensure that an initial sync attempt was made and succeeded if the data directory was cleaned.
    let res = assert.commandWorked(node.adminCommand({replSetGetStatus: 1, initialSync: 1}));
    if (expectResync) {
        assert.eq(1, res.initialSyncStatus.initialSyncAttempts.length);
        assert.eq(0, res.initialSyncStatus.failedInitialSyncAttempts);
    } else {
        assert.eq(undefined, res.initialSyncStatus);
    }

    testFunc(node);
    return node;
};

/**
 * Assert certain error messages are thrown on startup when files are missing or corrupt.
 */
let assertErrorOnStartupWhenFilesAreCorruptOrMissing = function(
    dbpath, dbName, collName, deleteOrCorruptFunc, errmsg) {
    // Start a MongoDB instance, create the collection file.
    const mongod = MongoRunner.runMongod({dbpath: dbpath, cleanData: true});
    const testColl = mongod.getDB(dbName)[collName];
    const doc = {a: 1};
    assert.commandWorked(testColl.insert(doc));

    // Stop MongoDB and corrupt/delete certain files.
    deleteOrCorruptFunc(mongod, testColl);

    // Restart the MongoDB instance and get an expected error message.
    clearRawMongoProgramOutput();
    assert.eq(MongoRunner.EXIT_ABRUPT,
              runMongoProgram("mongod", "--port", mongod.port, "--dbpath", dbpath));
    assert.gte(rawMongoProgramOutput().indexOf(errmsg), 0);
};

/**
 * Assert certain error messages are thrown on a specific request when files are missing or corrupt.
 */
let assertErrorOnRequestWhenFilesAreCorruptOrMissing = function(
    dbpath, dbName, collName, deleteOrCorruptFunc, requestFunc, errmsg) {
    // Start a MongoDB instance, create the collection file.
    mongod = MongoRunner.runMongod({dbpath: dbpath, cleanData: true});
    testColl = mongod.getDB(dbName)[collName];
    const doc = {a: 1};
    assert.commandWorked(testColl.insert(doc));

    // Stop MongoDB and corrupt/delete certain files.
    deleteOrCorruptFunc(mongod, testColl);

    // Restart the MongoDB instance.
    clearRawMongoProgramOutput();
    mongod = MongoRunner.runMongod({dbpath: dbpath, port: mongod.port, noCleanData: true});

    // This request crashes the server.
    testColl = mongod.getDB(dbName)[collName];
    requestFunc(testColl);

    // Get an expected error message.
    assert.gte(rawMongoProgramOutput().indexOf(errmsg), 0);
    MongoRunner.stopMongod(mongod, 9, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
};
