/*
 * Generally test that replica sets still function normally with mixed versions and mixed storage
 * engines. This test will set up a replica set containing members of various versions and
 * storage engines, do a bunch of random work, and assert that it replicates the same way on all
 * nodes.
 */

// This test randomly generates operations, which may include direct writes against
// config.transactions, which are not allowed to run under a session.
TestData.disableImplicitSessions = true;

load('jstests/libs/parallelTester.js');
load("jstests/replsets/rslib.js");

// Seed random numbers and print the seed. To reproduce a failed test, look for the seed towards
// the beginning of the output, and give it as an argument to randomize.
Random.setRandomSeed();

// Version constants.
const lastStableFCV = "3.6";

/*
 * Namespace for all random operation helpers. Actual tests start below
 */
var RandomOps = {
    // Change this to print all operations run.
    verbose: false,
    // 'Random' documents will have various combinations of these names mapping to these values
    fieldNames: ["a", "b", "c", "longerName", "numbered10", "dashed-name"],
    fieldValues: [
        true,
        false,
        0,
        44,
        -123,
        "",
        "String",
        [],
        [false, "x"],
        ["array", 1, {doc: true}, new Date().getTime()],
        {},
        {embedded: "document", weird: ["values", 0, false]},
        new Date().getTime()
    ],

    /*
     * Return a random element from Array a.
     */
    randomChoice: function(a) {
        if (a.length === 0) {
            print("randomChoice called on empty input!");
            return null;
        }
        var x = Random.rand();
        while (x === 1.0) {  // Would be out of bounds
            x = Random.rand();
        }
        var i = Math.floor(x * a.length);
        return a[i];
    },

    /*
     * Uses above arrays to create a new doc with a random amount of fields mapping to random
     * values.
     */
    randomNewDoc: function() {
        var doc = {};
        for (var i = 0; i < Random.randInt(0, this.fieldNames.length); i++) {
            doc[this.randomChoice(this.fieldNames)] = this.randomChoice(this.fieldValues);
        }
        return doc;
    },

    /*
     * Returns the names of all 'user created' (non admin/local) databases which have some data in
     * them, or an empty list if none exist.
     */
    getCreatedDatabases: function(conn) {
        var created = [];
        var dbs = conn.getDBs().databases;
        for (var i in dbs) {
            var db = dbs[i];
            if (db.name !== 'local' && db.name !== 'admin' && db.empty === false) {
                created.push(db.name);
            }
        }
        return created;
    },

    getRandomDoc: function(collection) {
        try {
            var randIndex = Random.randInt(0, collection.find().count());
            return collection.find().sort({$natural: 1}).skip(randIndex).limit(1)[0];
        } catch (e) {
            return undefined;
        }
    },

    /*
     * Returns a random user defined collection.
     *
     * The second parameter is a function that should return false if it wants to filter out
     * a collection from the list.
     *
     * If no collections exist, this returns null.
     */
    getRandomExistingCollection: function(conn, filterFn) {
        var matched = [];
        var dbs = this.getCreatedDatabases(conn);
        for (var i in dbs) {
            var dbName = dbs[i];
            var colls = conn.getDB(dbName)
                            .getCollectionNames()
                            .filter(function(collName) {
                                if (collName == "system.indexes") {
                                    return false;
                                } else if (filterFn && !filterFn(dbName, collName)) {
                                    return false;
                                } else {
                                    return true;
                                }
                            })
                            .map(function(collName) {
                                return conn.getDB(dbName).getCollection(collName);
                            });
            Array.prototype.push.apply(matched, colls);
        }
        if (matched.length === 0) {
            return null;
        }
        return this.randomChoice(matched);
    },

    //////////////////////////////////////////////////////////////////////////////////
    // RANDOMIZED CRUD OPERATIONS
    //////////////////////////////////////////////////////////////////////////////////

    /*
     * Insert a random document into a random collection, with a random writeconcern
     */
    insert: function(conn) {
        var databases = ["tic", "tac", "toe"];
        var collections = ["eeny", "meeny", "miny", "moe"];
        var writeConcerns = [-1, 0, 1, 2, 3, 4, 5, 6, "majority"];

        var db = this.randomChoice(databases);
        var coll = this.randomChoice(collections);
        var doc = this.randomNewDoc();
        if (Random.rand() < 0.5) {
            doc._id = new ObjectId().str;  // Vary whether or not we include the _id
        }
        var writeConcern = this.randomChoice(writeConcerns);
        var journal = this.randomChoice([true, false]);
        if (this.verbose) {
            print("Inserting: ");
            printjson(doc);
            print("With write concern: " + writeConcern + " and journal: " + journal);
        }
        var result =
            conn.getDB(db)[coll].insert(doc, {writeConcern: {w: writeConcern}, journal: journal});
        assert.writeOK(result);
        if (this.verbose) {
            print("done.");
        }
    },

    /*
     * remove a random document from a random collection
     */
    remove: function(conn) {
        var coll = this.getRandomExistingCollection(conn);
        if (coll === null || coll.find().count() === 0) {
            return null;  // No data, can't delete anything.
        }
        var doc = this.getRandomDoc(coll);
        if (doc === undefined) {
            // If multithreaded, there could have been issues finding a random doc.
            // If so, just skip this operation.
            return;
        }

        if (this.verbose) {
            print("Deleting:");
            printjson(doc);
        }
        try {
            coll.remove(doc);
        } catch (e) {
            if (this.verbose) {
                print("Caught exception in remove: " + e);
            }
        }
        if (this.verbose) {
            print("done.");
        }
    },

    /*
     * Update a random document from a random collection. Set a random field to a (possibly) new
     * value.
     */
    update: function(conn) {
        var coll = this.getRandomExistingCollection(conn);
        if (coll === null || coll.find().count() === 0) {
            return null;  // No data, can't update anything.
        }
        var doc = this.getRandomDoc(coll);
        if (doc === undefined) {
            // If multithreaded, there could have been issues finding a random doc.
            // If so, just skip this operation.
            return;
        }

        var field = this.randomChoice(this.fieldNames);
        var updateDoc = {$set: {}};
        updateDoc.$set[field] = this.randomChoice(this.fieldValues);
        if (this.verbose) {
            print("Updating:");
            printjson(doc);
            print("with:");
            printjson(updateDoc);
        }
        // If multithreaded, doc might not exist anymore.
        try {
            coll.update(doc, updateDoc);
        } catch (e) {
            if (this.verbose) {
                print("Caught exception in update: " + e);
            }
        }
        if (this.verbose) {
            print("done.");
        }
    },

    //////////////////////////////////////////////////////////////////////////////////
    // RANDOMIZED COMMANDS
    //////////////////////////////////////////////////////////////////////////////////

    /*
     * Randomly rename a collection to a new name. New name will be an ObjectId string.
     */
    renameCollection: function(conn) {
        var coll = this.getRandomExistingCollection(conn);
        if (coll === null) {
            return null;
        }
        var newName = coll.getDB().getName() + "." + new ObjectId().str;
        if (this.verbose) {
            print("renaming collection " + coll.getFullName() + " to " + newName);
        }
        assert.commandWorked(
            conn.getDB("admin").runCommand({renameCollection: coll.getFullName(), to: newName}));
        if (this.verbose) {
            print("done.");
        }
    },

    /*
     * Randomly drop a user created collection
     */
    dropCollection: function(conn) {
        var coll = this.getRandomExistingCollection(conn);
        if (coll === null) {
            return null;
        }
        if (this.verbose) {
            print("Dropping collection " + coll.getFullName());
        }
        assert.commandWorked(coll.runCommand({drop: coll.getName()}));
        if (this.verbose) {
            print("done.");
        }
    },

    /*
     * Randomly create an index on a random field in a random user defined collection.
     */
    createIndex: function(conn) {
        var coll = this.getRandomExistingCollection(conn);
        if (coll === null) {
            return null;
        }
        var index = {};
        index[this.randomChoice(this.fieldNames)] = this.randomChoice([-1, 1]);
        if (this.verbose) {
            print("Adding index " + tojsononeline(index) + " to " + coll.getFullName());
        }
        coll.ensureIndex(index);
        if (this.verbose) {
            print("done.");
        }
    },

    /*
     * Randomly drop one existing index on a random user defined collection
     */
    dropIndex: function(conn) {
        var coll = this.getRandomExistingCollection(conn);
        if (coll === null) {
            return null;
        }
        var index = this.randomChoice(coll.getIndices());
        if (index.name === "_id_") {
            return null;  // Don't drop that one.
        }
        if (this.verbose) {
            print("Dropping index " + tojsononeline(index.key) + " from " + coll.getFullName());
        }
        assert.commandWorked(coll.dropIndex(index.name));
        if (this.verbose) {
            print("done.");
        }
    },

    /*
     * Select a random collection and flip the user flag for usePowerOf2Sizes
     */
    collMod: function(conn) {
        var coll = this.getRandomExistingCollection(conn);
        if (coll === null) {
            return null;
        }
        var toggle = !coll.stats().userFlags;
        if (this.verbose) {
            print("Modifying usePowerOf2Sizes to " + toggle + " on collection " +
                  coll.getFullName());
        }
        coll.runCommand({collMod: coll.getName(), usePowerOf2Sizes: toggle});
        if (this.verbose) {
            print("done.");
        }
    },

    /*
     * Select a random user-defined collection and empty it
     */
    emptyCapped: function(conn) {
        var isCapped = function(dbName, coll) {
            return conn.getDB(dbName)[coll].isCapped();
        };
        var coll = this.getRandomExistingCollection(conn, isCapped);
        if (coll === null) {
            return null;
        }
        if (this.verbose) {
            print("Emptying capped collection: " + coll.getFullName());
        }
        assert.commandWorked(coll.runCommand({emptycapped: coll.getName()}));
        if (this.verbose) {
            print("done.");
        }
    },

    /*
     * Apply some ops to a random collection. For now we'll just have insert ops.
     */
    applyOps: function(conn) {
        // Check if there are any valid collections to choose from.
        if (this.getRandomExistingCollection(conn) === null) {
            return null;
        }
        var ops = [];
        // Insert between 1 and 10 things.
        for (var i = 0; i < Random.randInt(1, 10); i++) {
            var coll = this.getRandomExistingCollection(conn);
            var doc = this.randomNewDoc();
            doc._id = new ObjectId();
            if (coll !== null) {
                ops.push({op: "i", ns: coll.getFullName(), o: doc});
            }
        }
        if (this.verbose) {
            print("Applying the following ops: ");
            printjson(ops);
        }
        assert.commandWorked(conn.getDB("admin").runCommand({applyOps: ops}));
        if (this.verbose) {
            print("done.");
        }
    },

    /*
     * Create a random collection. Use an ObjectId for the name
     */
    createCollection: function(conn) {
        var dbs = this.getCreatedDatabases(conn);
        if (dbs.length === 0) {
            return null;
        }
        var dbName = this.randomChoice(dbs);
        var newName = new ObjectId().str;
        if (this.verbose) {
            print("Creating new collection: " + "dbName" + "." + newName);
        }
        assert.commandWorked(conn.getDB(dbName).runCommand({create: newName}));
        if (this.verbose) {
            print("done.");
        }
    },

    /*
     * Convert a random non-capped collection to a capped one with size 1MB.
     */
    convertToCapped: function(conn) {
        var isNotCapped = function(dbName, coll) {
            return !conn.getDB(dbName)[coll].isCapped();
        };
        var coll = this.getRandomExistingCollection(conn, isNotCapped);
        if (coll === null) {
            return null;
        }
        if (this.verbose) {
            print("Converting " + coll.getFullName() + " to a capped collection.");
        }
        assert.commandWorked(coll.runCommand({convertToCapped: coll.getName(), size: 1024 * 1024}));
        if (this.verbose) {
            print("done.");
        }
    },

    appendOplogNote: function(conn) {
        var note = "Test note " + new ObjectId().str;
        if (this.verbose) {
            print("Appending oplog note: " + note);
        }
        assert.commandWorked(
            conn.getDB("admin").runCommand({appendOplogNote: note, data: {some: 'doc'}}));
        if (this.verbose) {
            print("done.");
        }
    },

    /*
     * Repeatedly call methods numOps times, choosing randomly from possibleOps, which should be
     * a list of strings representing method names of the RandomOps object.
     */
    doRandomWork: function(conn, numOps, possibleOps) {
        for (var i = 0; i < numOps; i++) {
            op = this.randomChoice(possibleOps);
            try {
                this[op](conn);
            } catch (ex) {
                print('doRandomWork - ' + op + ': failed: ' + ex);
                throw ex;
            }
        }
    }

};  // End of RandomOps

//////////////////////////////////////////////////////////////////////////////////
// OTHER HELPERS
//////////////////////////////////////////////////////////////////////////////////

function isArbiter(conn) {
    return conn.adminCommand({isMaster: 1}).arbiterOnly === true;
}

function removeFromArray(elem, a) {
    a.splice(a.indexOf(elem), 1);
}

/*
 * builds a function to be passed to assert.soon. Needs to know which node to expect as the new
 * primary
 */
var primaryChanged = function(conns, replTest, primaryIndex) {
    return function() {
        return conns[primaryIndex] == replTest.getPrimary();
    };
};

/*
 * If we have determined a collection doesn't match on two hosts, use this to get a string of the
 * differences.
 */
function getCollectionDiff(db1, db2, collName) {
    var coll1 = db1[collName];
    var coll2 = db2[collName];
    var cur1 = coll1.find().sort({$natural: 1});
    var cur2 = coll2.find().sort({$natural: 1});
    var diffText = "";
    while (cur1.hasNext() && cur2.hasNext()) {
        var doc1 = cur1.next();
        var doc2 = cur2.next();
        if (doc1 != doc2) {
            diffText += "mismatching doc:" + tojson(doc1) + tojson(doc2);
        }
    }
    if (cur1.hasNext()) {
        diffText += db1.getMongo().host + " has extra documents:";
        while (cur1.hasNext()) {
            diffText += "\n" + tojson(cur1.next());
        }
    }
    if (cur2.hasNext()) {
        diffText += db2.getMongo().host + " has extra documents:";
        while (cur2.hasNext()) {
            diffText += "\n" + tojson(cur2.next());
        }
    }
    return diffText;
}

/*
 * Check if two databases are equal. If not, print out what the differences are to aid with
 * debugging.
 */
function assertDBsEq(db1, db2) {
    assert.eq(db1.getName(), db2.getName());
    var hash1 = db1.runCommand({dbHash: 1});
    var hash2 = db2.runCommand({dbHash: 1});
    var host1 = db1.getMongo().host;
    var host2 = db2.getMongo().host;
    var success = true;
    var collNames1 = db1.getCollectionNames();
    var collNames2 = db2.getCollectionNames();
    var diffText = "";
    if (db1.getName() === 'local') {
        // We don't expect the entire local collection to be the same, not even the oplog, since
        // it's a capped collection.
        return;
    } else if (hash1.md5 != hash2.md5) {
        for (var i = 0; i < Math.min(collNames1.length, collNames2.length); i++) {
            var collName = collNames1[i];
            if (collName.startsWith('system.')) {
                // Skip system collections. These are not included in the dbhash before 3.3.10.
                continue;
            }
            if (hash1.collections[collName] !== hash2.collections[collName]) {
                if (db1[collName].stats().capped) {
                    if (!db2[collName].stats().capped) {
                        success = false;
                        diffText +=
                            "\n" + collName + " is capped on " + host1 + " but not on " + host2;
                    } else {
                        // Skip capped collections. They are not expected to be the same from host
                        // to host.
                        continue;
                    }
                } else {
                    success = false;
                    diffText +=
                        "\n" + collName + " differs: " + getCollectionDiff(db1, db2, collName);
                }
            }
        }
    }
    assert.eq(success,
              true,
              "Database " + db1.getName() + " differs on " + host1 + " and " + host2 +
                  "\nCollections: " + collNames1 + " vs. " + collNames2 + "\n" + diffText);
}

/*
 * Check the database hashes of all databases to ensure each node of the replica set has the same
 * data.
 */
function assertSameData(primary, conns) {
    var dbs = primary.getDBs().databases;
    for (var i in dbs) {
        var db1 = primary.getDB(dbs[i].name);
        for (var j in conns) {
            var conn = conns[j];
            if (!isArbiter(conn)) {
                var db2 = conn.getDB(dbs[i].name);
                assertDBsEq(db1, db2);
            }
        }
    }
}

/*
 * function to pass to a thread to make it start doing random commands/CRUD operations.
 */
function startCmds(randomOps, host) {
    // This test randomly generates operations, which may include direct writes against
    // config.transactions, which are not allowed to run under a session.
    TestData = {disableImplicitSessions: true};

    var ops = [
        "insert",
        "remove",
        "update",
        "renameCollection",
        "dropCollection",
        "createIndex",
        "dropIndex",
        "collMod",
        "emptyCapped",
        "applyOps",
        "createCollection",
        "convertToCapped",
        "appendOplogNote"
    ];
    var m = new Mongo(host);
    var numOps = 200;
    Random.setRandomSeed();
    randomOps.doRandomWork(m, numOps, ops);
    return true;
}

/*
 * function to pass to a thread to make it start doing random CRUD operations.
 */
function startCRUD(randomOps, host) {
    // This test randomly generates operations, which may include direct writes against
    // config.transactions, which are not allowed to run under a session.
    TestData = {disableImplicitSessions: true};

    var m = new Mongo(host);
    var numOps = 500;
    Random.setRandomSeed();
    randomOps.doRandomWork(m, numOps, ["insert", "update", "remove"]);
    return true;
}

/*
 * To avoid race conditions on things like trying to drop a collection while another thread is
 * trying to rename it, just have one thread that might issue commands, and the others do random
 * CRUD operations. To be clear, this is something that the Mongod should be able to handle, but
 * this test code does not have atomic random operations. E.g. it has to first randomly select
 * a collection to drop an index from, which may not be there by the time it tries to get a list
 * of indices on the collection.
 */
function doMultiThreadedWork(primary, numThreads) {
    var threads = [];
    // The command thread
    // Note we pass the hostname, as we have to re-establish the connection in the new thread.
    var cmdThread = new ScopedThread(startCmds, RandomOps, primary.host);
    threads.push(cmdThread);
    cmdThread.start();
    // Other CRUD threads
    for (var i = 1; i < numThreads; i++) {
        var crudThread = new ScopedThread(startCRUD, RandomOps, primary.host);
        threads.push(crudThread);
        crudThread.start();
    }
    for (var j = 0; j < numThreads; j++) {
        assert.eq(threads[j].returnData(), true);
    }
}

//////////////////////////////////////////////////////////////////////////////////
// START ACTUAL TESTING
//////////////////////////////////////////////////////////////////////////////////

(function() {
    "use strict";
    var name = "mixed_storage_and_version";
    // Create a replica set with 2 nodes of each of the types below, plus one arbiter.
    var oldVersion = "last-stable";
    var newVersion = "latest";

    var setups = [
        {binVersion: newVersion, storageEngine: 'mmapv1'},
        {binVersion: newVersion, storageEngine: 'mmapv1'},
        {binVersion: newVersion, storageEngine: 'wiredTiger'},
        {binVersion: newVersion, storageEngine: 'wiredTiger'},
        {binVersion: oldVersion},
        {binVersion: oldVersion},
        {arbiter: true},
    ];
    var replTest = new ReplSetTest({nodes: {n0: setups[0]}, name: name});
    replTest.startSet();
    var config = replTest.getReplSetConfig();
    // Override the default value -1 in 3.5.
    config.settings = {catchUpTimeoutMillis: 2000};
    replTest.initiate(config);

    // We set the featureCompatibilityVersion to lastStableFCV so that last-stable binary version
    // secondaries can successfully initial sync from a latest binary version primary. We do this
    // prior to adding any other members to the replica set. This effectively allows us to emulate
    // upgrading some of our nodes to the latest version while different last-stable version and
    // latest version mongod processes are being elected primary.
    assert.commandWorked(
        replTest.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

    for (let i = 1; i < setups.length; ++i) {
        replTest.add(setups[i]);
    }

    var newConfig = replTest.getReplSetConfig();
    config = replTest.getReplSetConfigFromNode();
    // Make sure everyone is syncing from the primary, to ensure we have all combinations of
    // primary/secondary syncing.
    config.members = newConfig.members;
    config.settings.chainingAllowed = false;
    config.version += 1;
    reconfig(replTest, config);

    // Ensure all are synced.
    replTest.awaitSecondaryNodes(120000);
    var primary = replTest.getPrimary();

    Random.setRandomSeed();

    // Keep track of the indices of different types of primaries.
    // We'll rotate to get a primary of each type.
    var possiblePrimaries = [0, 2, 4];
    var highestPriority = 2;
    while (possiblePrimaries.length > 0) {
        config = primary.getDB("local").system.replset.findOne();
        var primaryIndex = RandomOps.randomChoice(possiblePrimaries);
        print("TRANSITIONING to " + tojsononeline(setups[primaryIndex / 2]) + " as primary");
        // Remove chosen type from future choices.
        removeFromArray(primaryIndex, possiblePrimaries);
        config.members[primaryIndex].priority = highestPriority;
        if (config.version === undefined) {
            config.version = 2;
        } else {
            config.version++;
        }
        highestPriority++;
        printjson(config);
        reconfig(replTest, config);
        replTest.awaitReplication();
        assert.soon(primaryChanged(replTest.nodes, replTest, primaryIndex),
                    "waiting for higher priority primary to be elected",
                    100000);
        print("New primary elected, doing a bunch of work");
        primary = replTest.getPrimary();
        doMultiThreadedWork(primary, 10);
        replTest.awaitReplication();
        print("Work done, checking to see all nodes match");
        assertSameData(primary, replTest.nodes);
    }
    replTest.stopSet();
})();
