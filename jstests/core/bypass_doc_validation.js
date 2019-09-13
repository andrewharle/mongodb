// @tags: [does_not_support_stepdowns, requires_non_retryable_commands, requires_fastcount]

/**
 * Tests that various database commands respect the 'bypassDocumentValidation' flag:
 *
 * - aggregation with $out
 * - applyOps (when not sharded)
 * - copyDb
 * - doTxn (when not sharded)
 * - findAndModify
 * - insert
 * - mapReduce
 * - update
 */
(function() {
    'use strict';

    // For isWiredTiger.
    load("jstests/concurrency/fsm_workload_helpers/server_types.js");
    // For isReplSet
    load("jstests/libs/fixture_helpers.js");

    function assertFailsValidation(res) {
        if (res instanceof WriteResult || res instanceof BulkWriteResult) {
            assert.writeErrorWithCode(res, ErrorCodes.DocumentValidationFailure, tojson(res));
        } else {
            assert.commandFailedWithCode(res, ErrorCodes.DocumentValidationFailure, tojson(res));
        }
    }

    const dbName = 'bypass_document_validation';
    const collName = 'bypass_document_validation';
    const myDb = db.getSiblingDB(dbName);
    const coll = myDb[collName];

    /**
     * Tests that we can bypass document validation when appropriate when a collection has validator
     * 'validator', which should enforce the existence of a field "a".
     */
    function runBypassDocumentValidationTest(validator) {
        // Use majority write concern to clear the drop-pending that can cause lock conflicts with
        // transactions.
        coll.drop({writeConcern: {w: "majority"}});

        // Insert documents into the collection that would not be valid before setting 'validator'.
        assert.writeOK(coll.insert({_id: 1}));
        assert.writeOK(coll.insert({_id: 2}));
        assert.commandWorked(myDb.runCommand({collMod: collName, validator: validator}));

        const isMongos = db.runCommand({isdbgrid: 1}).isdbgrid;
        // Test applyOps with a simple insert if not on mongos.
        if (!isMongos) {
            const op = [{op: 'i', ns: coll.getFullName(), o: {_id: 9}}];
            assertFailsValidation(myDb.runCommand({applyOps: op, bypassDocumentValidation: false}));
            assert.eq(0, coll.count({_id: 9}));
            assert.commandWorked(myDb.runCommand({applyOps: op, bypassDocumentValidation: true}));
            assert.eq(1, coll.count({_id: 9}));
        }

        // Test doTxn with a simple insert if a replica set, not on mongos and the storage engine
        // is WiredTiger.
        if (FixtureHelpers.isReplSet(db) && !isMongos && isWiredTiger(db)) {
            const session = db.getMongo().startSession();
            const sessionDb = session.getDatabase(myDb.getName());
            const op = [{op: 'i', ns: coll.getFullName(), o: {_id: 10}}];
            assertFailsValidation(sessionDb.runCommand(
                {doTxn: op, bypassDocumentValidation: false, txnNumber: NumberLong("0")}));
            assert.eq(0, coll.count({_id: 10}));
            assert.commandWorked(sessionDb.runCommand(
                {doTxn: op, bypassDocumentValidation: true, txnNumber: NumberLong("1")}));
            assert.eq(1, coll.count({_id: 10}));
        }

        // Test the aggregation command with a $out stage.
        const outputCollName = 'bypass_output_coll';
        const outputColl = myDb[outputCollName];
        outputColl.drop();
        assert.commandWorked(myDb.createCollection(outputCollName, {validator: validator}));
        const pipeline =
            [{$match: {_id: 1}}, {$project: {aggregation: {$add: [1]}}}, {$out: outputCollName}];
        assert.throws(function() {
            coll.aggregate(pipeline, {bypassDocumentValidation: false});
        });
        assert.eq(0, outputColl.count({aggregation: 1}));
        coll.aggregate(pipeline, {bypassDocumentValidation: true});
        assert.eq(1, outputColl.count({aggregation: 1}));

        // Test the copyDb command.
        const copyDbName = dbName + '_copy';
        const copyDb = myDb.getSiblingDB(copyDbName);
        assert.commandWorked(copyDb.dropDatabase());
        let res = db.adminCommand(
            {copydb: 1, fromdb: dbName, todb: copyDbName, bypassDocumentValidation: false});
        assertFailsValidation(res);
        assert.eq(0, copyDb[collName].count());
        assert.commandWorked(copyDb.dropDatabase());
        assert.commandWorked(db.adminCommand(
            {copydb: 1, fromdb: dbName, todb: copyDbName, bypassDocumentValidation: true}));
        assert.eq(coll.count(), db.getSiblingDB(copyDbName)[collName].count());

        // Test the findAndModify command.
        assert.throws(function() {
            coll.findAndModify(
                {update: {$set: {findAndModify: 1}}, bypassDocumentValidation: false});
        });
        assert.eq(0, coll.count({findAndModify: 1}));
        coll.findAndModify({update: {$set: {findAndModify: 1}}, bypassDocumentValidation: true});
        assert.eq(1, coll.count({findAndModify: 1}));

        // Test the mapReduce command.
        const map = function() {
            emit(1, 1);
        };
        const reduce = function() {
            return 'mapReduce';
        };
        res = myDb.runCommand({
            mapReduce: collName,
            map: map,
            reduce: reduce,
            out: {replace: outputCollName},
            bypassDocumentValidation: false
        });
        assertFailsValidation(res);
        assert.eq(0, outputColl.count({value: 'mapReduce'}));
        res = myDb.runCommand({
            mapReduce: collName,
            map: map,
            reduce: reduce,
            out: {replace: outputCollName},
            bypassDocumentValidation: true
        });
        assert.commandWorked(res);
        assert.eq(1, outputColl.count({value: 'mapReduce'}));

        // Test the insert command. Includes a test for a document with no _id (SERVER-20859).
        res = myDb.runCommand({insert: collName, documents: [{}], bypassDocumentValidation: false});
        assertFailsValidation(BulkWriteResult(res));
        res = myDb.runCommand(
            {insert: collName, documents: [{}, {_id: 6}], bypassDocumentValidation: false});
        assertFailsValidation(BulkWriteResult(res));
        res = myDb.runCommand(
            {insert: collName, documents: [{}, {_id: 6}], bypassDocumentValidation: true});
        assert.writeOK(res);

        // Test the update command.
        res = myDb.runCommand({
            update: collName,
            updates: [{q: {}, u: {$set: {update: 1}}}],
            bypassDocumentValidation: false
        });
        assertFailsValidation(BulkWriteResult(res));
        assert.eq(0, coll.count({update: 1}));
        res = myDb.runCommand({
            update: collName,
            updates: [{q: {}, u: {$set: {update: 1}}}],
            bypassDocumentValidation: true
        });
        assert.writeOK(res);
        assert.eq(1, coll.count({update: 1}));
    }

    // Run the test using a normal validator.
    runBypassDocumentValidationTest({a: {$exists: true}});

    // Run the test again with an equivalent JSON Schema validator.
    runBypassDocumentValidationTest({$jsonSchema: {required: ['a']}});
})();
