// Check that the dbhashes of all the nodes in a ReplSetTest are consistent.
'use strict';

function checkDBHashes(rst, dbBlacklist = [], phase = 'after test hook') {
    function generateUniqueDbName(dbNameSet, prefix) {
        var uniqueDbName;
        Random.setRandomSeed();
        do {
            uniqueDbName = prefix + Random.randInt(100000);
        } while (dbNameSet.has(uniqueDbName));
        return uniqueDbName;
    }

    // Return items that are in either Array `a` or `b` but not both. Note that this will not work
    // with arrays containing NaN. Array.indexOf(NaN) will always return -1.
    function arraySymmetricDifference(a, b) {
        var inAOnly = a.filter(function(elem) {
            return b.indexOf(elem) < 0;
        });

        var inBOnly = b.filter(function(elem) {
            return a.indexOf(elem) < 0;
        });

        return inAOnly.concat(inBOnly);
    }

    function dumpCollectionDiff(primary, secondary, dbName, collName) {
        print('Dumping collection: ' + dbName + '.' + collName);

        var primaryColl = primary.getDB(dbName).getCollection(collName);
        var secondaryColl = secondary.getDB(dbName).getCollection(collName);

        var primaryDocs = primaryColl.find().sort({_id: 1}).toArray();
        var secondaryDocs = secondaryColl.find().sort({_id: 1}).toArray();

        var primaryIndex = primaryDocs.length - 1;
        var secondaryIndex = secondaryDocs.length - 1;

        var missingOnPrimary = [];
        var missingOnSecondary = [];

        while (primaryIndex >= 0 || secondaryIndex >= 0) {
            var primaryDoc = primaryDocs[primaryIndex];
            var secondaryDoc = secondaryDocs[secondaryIndex];

            if (primaryIndex < 0) {
                missingOnPrimary.push(tojsononeline(secondaryDoc));
                secondaryIndex--;
            } else if (secondaryIndex < 0) {
                missingOnSecondary.push(tojsononeline(primaryDoc));
                primaryIndex--;
            } else {
                if (!bsonBinaryEqual(primaryDoc, secondaryDoc)) {
                    print('Mismatching documents:');
                    print('    primary: ' + tojsononeline(primaryDoc));
                    print('    secondary: ' + tojsononeline(secondaryDoc));
                    var ordering =
                        bsonWoCompare({wrapper: primaryDoc._id}, {wrapper: secondaryDoc._id});
                    if (ordering === 0) {
                        primaryIndex--;
                        secondaryIndex--;
                    } else if (ordering < 0) {
                        missingOnPrimary.push(tojsononeline(secondaryDoc));
                        secondaryIndex--;
                    } else if (ordering > 0) {
                        missingOnSecondary.push(tojsononeline(primaryDoc));
                        primaryIndex--;
                    }
                } else {
                    // Latest document matched.
                    primaryIndex--;
                    secondaryIndex--;
                }
            }
        }

        if (missingOnPrimary.length) {
            print('The following documents are missing on the primary:');
            print(missingOnPrimary.join('\n'));
        }
        if (missingOnSecondary.length) {
            print('The following documents are missing on the secondary:');
            print(missingOnSecondary.join('\n'));
        }
    }

    function checkDBHashesForReplSet(rst, dbBlacklist, phase) {
        // We don't expect the local database to match because some of its collections are not
        // replicated.
        dbBlacklist.push('local');

        var success = true;
        var hasDumpedOplog = false;

        // Use liveNodes.master instead of getPrimary() to avoid the detection of a new primary.
        // liveNodes must have been populated.
        var primary = rst.liveNodes.master;
        var combinedDBs = new Set(primary.getDBNames());

        rst.getSecondaries().forEach(secondary => {
            secondary.getDBNames().forEach(dbName => combinedDBs.add(dbName));
        });

        for (var dbName of combinedDBs) {
            if (Array.contains(dbBlacklist, dbName)) {
                continue;
            }

            var dbHashes = rst.getHashes(dbName);
            var primaryDBHash = dbHashes.master;
            assert.commandWorked(primaryDBHash);

            var primaryCollInfo = primary.getDB(dbName).getCollectionInfos();

            dbHashes.slaves.forEach(secondaryDBHash => {
                assert.commandWorked(secondaryDBHash);

                var secondary =
                    rst.liveNodes.slaves.find(node => node.host === secondaryDBHash.host);
                assert(secondary,
                       'could not find the replica set secondary listed in the dbhash response ' +
                           tojson(secondaryDBHash));

                var primaryCollections = Object.keys(primaryDBHash.collections);
                var secondaryCollections = Object.keys(secondaryDBHash.collections);

                if (primaryCollections.length !== secondaryCollections.length) {
                    print(phase +
                          ', the primary and secondary have a different number of collections: ' +
                          tojson(dbHashes));
                    for (var diffColl of
                             arraySymmetricDifference(primaryCollections, secondaryCollections)) {
                        dumpCollectionDiff(primary, secondary, dbName, diffColl);
                    }
                    success = false;
                }

                var nonCappedCollNames = primaryCollections.filter(
                    collName => !primary.getDB(dbName).getCollection(collName).isCapped());
                // Only compare the dbhashes of non-capped collections because capped collections
                // are not necessarily truncated at the same points across replica set members.
                nonCappedCollNames.forEach(collName => {
                    if (primaryDBHash.collections[collName] !==
                        secondaryDBHash.collections[collName]) {
                        print(phase + ', the primary and secondary have a different hash for the' +
                              ' collection ' + dbName + '.' + collName + ': ' + tojson(dbHashes));
                        dumpCollectionDiff(primary, secondary, dbName, collName);
                        success = false;
                    }

                });

                // Check that collection information is consistent on the primary and secondaries.
                var secondaryCollInfo = secondary.getDB(dbName).getCollectionInfos();
                secondaryCollInfo.forEach(secondaryInfo => {
                    primaryCollInfo.forEach(primaryInfo => {
                        if (secondaryInfo.name === primaryInfo.name) {
                            if (!bsonBinaryEqual(secondaryInfo, primaryInfo)) {
                                print(phase +
                                      ', the primary and secondary have different attributes for ' +
                                      'the collection ' + dbName + '.' + secondaryInfo.name);
                                print('Collection info on the primary: ' + tojson(primaryInfo));
                                print('Collection info on the secondary: ' + tojson(secondaryInfo));
                                success = false;
                            }
                        }
                    });
                });

                // Check that the following collection stats are the same across replica set
                // members:
                //  capped
                //  nindexes
                //  ns
                primaryCollections.forEach(collName => {
                    var primaryCollStats = primary.getDB(dbName).runCommand({collStats: collName});
                    assert.commandWorked(primaryCollStats);
                    var secondaryCollStats =
                        secondary.getDB(dbName).runCommand({collStats: collName});
                    assert.commandWorked(secondaryCollStats);

                    if (primaryCollStats.capped !== secondaryCollStats.capped ||
                        primaryCollStats.nindexes !== secondaryCollStats.nindexes ||
                        primaryCollStats.ns !== secondaryCollStats.ns) {
                        print(phase + ', the primary and secondary have different stats for the ' +
                              'collection ' + dbName + '.' + collName);
                        print('Collection stats on the primary: ' + tojson(primaryCollStats));
                        print('Collection stats on the secondary: ' + tojson(secondaryCollStats));
                        success = false;
                    }
                });

                if (nonCappedCollNames.length === primaryCollections.length) {
                    // If the primary and secondary have the same hashes for all the collections in
                    // the database and there aren't any capped collections, then the hashes for
                    // the whole database should match.
                    if (primaryDBHash.md5 !== secondaryDBHash.md5) {
                        print(phase + ', the primary and secondary have a different hash for ' +
                              'the ' + dbName + ' database: ' + tojson(dbHashes));
                        success = false;
                    }
                }

                if (!success) {
                    var dumpOplog = function(conn, limit) {
                        print('Dumping the latest ' + limit + ' documents from the oplog of ' +
                              conn.host);
                        var cursor = conn.getDB('local')
                                         .getCollection('oplog.rs')
                                         .find()
                                         .sort({$natural: -1})
                                         .limit(limit);
                        cursor.forEach(printjsononeline);
                    };

                    if (!hasDumpedOplog) {
                        dumpOplog(primary, 100);
                        rst.getSecondaries().forEach(secondary => dumpOplog(secondary, 100));
                        hasDumpedOplog = true;
                    }
                }
            });
        }

        assert(success, 'dbhash mismatch between primary and secondary');
    }

    // Call getPrimary to populate rst with information about the nodes.
    var primary = rst.getPrimary();
    assert(primary, 'calling getPrimary() failed');

    // Since we cannot determine if there is a background index in progress (SERVER-25176),
    // we flush indexing as follows:
    //  1. Create a foreground index on a dummy collection/database
    //  2. Insert a document into the dummy collection with a writeConcern for all nodes
    //  3. Drop the dummy database
    var dbNames = new Set(primary.getDBNames());
    var uniqueDbName = generateUniqueDbName(dbNames, "flush_all_background_indexes_");

    var dummyDB = primary.getDB(uniqueDbName);
    var dummyColl = dummyDB.dummy;
    dummyColl.drop();
    assert.commandWorked(dummyColl.createIndex({x: 1}));
    assert.writeOK(dummyColl.insert(
        {x: 1}, {writeConcern: {w: rst.nodeList().length, wtimeout: 5 * 60 * 1000}}));
    assert.commandWorked(dummyDB.dropDatabase());

    var activeException = false;

    try {
        // dbHash values for collections in the "config" database are cached and may be stale due to
        // SERVER-22156. We insert a document into each of these collections to invalidate
        // the potentially stale cache entries.
        var primaryConfigDb = rst.liveNodes.master.getDB('config');
        for (var collName of primaryConfigDb.getCollectionNames()) {
            // Invalidate the dbhash cache for non-system collections and user-writable system
            // collections.
            if (!collName.startsWith('system.') || collName === 'system.js' ||
                collName === 'system.users')
                // This insert can fail if there's a unique index on the collection, so we can't
                // check the return value.
                primaryConfigDb.getCollection(collName).insert({invalidate: 'cache'});
        }

        // Lock the primary to prevent the TTL monitor from deleting expired documents in
        // the background while we are getting the dbhashes of the replica set members.
        assert.commandWorked(primary.adminCommand({fsync: 1, lock: 1}),
                             'failed to lock the primary');
        rst.awaitReplication(60 * 1000 * 5);
        checkDBHashesForReplSet(rst, dbBlacklist, phase);
    } catch (e) {
        activeException = true;
        throw e;
    } finally {
        // Allow writes on the primary.
        var res = primary.adminCommand({fsyncUnlock: 1});

        if (!res.ok) {
            var msg = 'failed to unlock the primary, which may cause this' +
                ' test to hang: ' + tojson(res);
            if (activeException) {
                print(msg);
            } else {
                throw new Error(msg);
            }
        }
    }
}
