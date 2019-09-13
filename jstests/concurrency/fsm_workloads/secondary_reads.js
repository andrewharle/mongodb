'use strict';

/**
 * secondary_reads.js
 *
 * One thread (tid 0) is dedicated to writing documents with field 'x' in
 * ascending order into the collection.
 *
 * Other threads do one of the following operations each iteration.
 * 1) Retrieve first 50 documents in descending order with local readConcern from a secondary node.
 * 2) Retrieve first 50 documents in descending order with available readConcern from a secondary
 * node.
 * 3) Retrieve first 50 documents in descending order with majority readConcern from a secondary
 * node.
 *
 * For each read, we check if there is any 'hole' in the returned batch. There
 * should not be any 'hole' because oplogs are applied sequentially in batches.
 *
 * @tags: [requires_replication]
 */

var $config = (function() {

    // Use the workload name as the collection name.
    var uniqueCollectionName = 'secondary_reads';

    load('jstests/concurrency/fsm_workload_helpers/server_types.js');

    function isWriterThread() {
        return this.tid === 0;
    }

    function insertDocuments(db, collName, writeConcern) {
        let bulk = db[collName].initializeOrderedBulkOp();
        for (let i = this.nDocumentsInTotal; i < this.nDocumentsInTotal + this.nDocumentsToInsert;
             i++) {
            bulk.insert({_id: i, x: i});
        }
        let res = bulk.execute(writeConcern);
        assertWhenOwnColl.writeOK(res);
        assertWhenOwnColl.eq(this.nDocumentsToInsert, res.nInserted);
        this.nDocumentsInTotal += this.nDocumentsToInsert;
    }

    function readFromSecondaries(db, collName, readConcernLevel) {
        let arr = [];
        let success = false;
        while (!success) {
            try {
                arr = db[collName]
                          .find()
                          .readPref('secondary')
                          .readConcern(readConcernLevel)
                          .sort({x: -1})
                          .limit(this.nDocumentsToCheck)
                          .toArray();
                success = true;
            } catch (e) {
                // We propagate TransientTransactionErrors to allow the state function to
                // automatically be retried when TestData.runInsideTransaction=true
                if (e.hasOwnProperty('errorLabels') &&
                    e.errorLabels.includes('TransientTransactionError')) {
                    throw e;
                }
                // Retry if the query is interrupted.
                assertAlways.eq(e.code,
                                ErrorCodes.QueryPlanKilled,
                                'unexpected error code: ' + e.code + ': ' + e.message);
            }
        }
        // Make sure there is no hole in the result.
        for (let i = 0; i < arr.length - 1; i++) {
            assertWhenOwnColl.eq(arr[i].x, arr[i + 1].x + 1, () => tojson(arr));
        }
    }

    function getReadConcernLevel(supportsCommittedReads) {
        const readConcernLevels = ['local'];
        if (!TestData.runningWithCausalConsistency && !TestData.runInsideTransaction) {
            readConcernLevels.push('available');
        }
        if (supportsCommittedReads) {
            readConcernLevels.push('majority');
        }
        return readConcernLevels[Random.randInt(readConcernLevels.length)];
    }

    var states = (function() {

        // One thread is dedicated to writing and other threads perform reads on
        // secondaries with a randomly chosen readConcern level.
        function readFromSecondaries(db, collName) {
            if (this.isWriterThread()) {
                this.insertDocuments(db, this.collName);
            } else {
                this.readFromSecondaries(
                    db, this.collName, getReadConcernLevel(supportsCommittedReads(db)));
            }
        }

        return {readFromSecondaries: readFromSecondaries};
    })();

    var transitions = {readFromSecondaries: {readFromSecondaries: 1}};

    var setup = function setup(db, collName, cluster) {
        this.nDocumentsInTotal = 0;
        // Start write workloads to activate oplog application on secondaries
        // before any reads.
        this.insertDocuments(db, this.collName, {w: cluster.getReplSetNumNodes()});
        // Facilitate sorting by "x".
        db[collName].createIndex({x: 1});
    };

    return {
        threadCount: 30,
        iterations: 10,
        startState: 'readFromSecondaries',
        states: states,
        data: {
            nDocumentsToInsert: 2000,
            nDocumentsToCheck: 50,
            isWriterThread: isWriterThread,
            insertDocuments: insertDocuments,
            readFromSecondaries: readFromSecondaries,
            collName: uniqueCollectionName
        },
        transitions: transitions,
        setup: setup,
    };
})();
