// SERVER-7200 On startup, replica set nodes delete oplog state past the oplog delete point and
// apply any remaining unapplied ops before coming up as a secondary.
//
// This test requires mmapv1 because rollback to a stable timestamp does not allow arbitrary
// writes to the minValid document. This has been replaced by unittests.
// @tags: [requires_persistence, requires_mmapv1]
(function() {
    "use strict";

    var ns = "test.coll";

    var rst = new ReplSetTest({
        nodes: 1,
    });

    rst.startSet();
    rst.initiate();

    var conn = rst.getPrimary();  // Waits for PRIMARY state.
    var nojournal = Array.contains(conn.adminCommand({getCmdLineOpts: 1}).argv, '--nojournal');
    var storageEngine = jsTest.options().storageEngine;
    var term = conn.getCollection('local.oplog.rs').find().sort({$natural: -1}).limit(1).next().t;

    function runTest({
        oplogEntries,
        collectionContents,
        deletePoint,
        begin,
        minValid,
        expectedState,
        expectedApplied,
    }) {
        if (nojournal && (storageEngine === 'mmapv1') && expectedState === 'FATAL') {
            // We can't test fatal states on mmap without a journal because it won't be able
            // to start up again.
            return;
        }

        if (term != -1) {
            term++;  // Each test gets a new term on PV1 to ensure OpTimes always move forward.
        }

        conn = rst.restart(0, {noReplSet: true});  // Restart as a standalone node.
        assert.neq(null, conn, "failed to restart");
        var oplog = conn.getCollection('local.oplog.rs');
        var minValidColl = conn.getCollection('local.replset.minvalid');
        var oplogTruncateAfterColl = conn.getCollection('local.replset.oplogTruncateAfterPoint');
        var coll = conn.getCollection(ns);

        // Reset state to empty.
        assert.commandWorked(oplog.runCommand('emptycapped'));
        coll.drop();
        assert.commandWorked(coll.runCommand('create'));

        var ts = (num) => num === null ? Timestamp() : Timestamp(1000, num);

        oplogEntries.forEach((num) => {
            assert.writeOK(oplog.insert({
                ts: ts(num),
                t: NumberLong(term),
                h: NumberLong(1),
                op: 'i',
                ns: ns,
                v: 2,
                o: {_id: num},
            }));
        });

        collectionContents.forEach((num) => {
            assert.writeOK(coll.insert({_id: num}));
        });

        var injectedMinValidDoc = {
            _id: ObjectId(),

            // appliedThrough
            begin: {
                ts: ts(begin),
                t: NumberLong(term),
            },

            // minvalid:
            t: NumberLong(term),
            ts: ts(minValid),
        };

        var injectedOplogTruncateAfterPointDoc = {
            _id: "oplogTruncateAfterPoint",
            oplogTruncateAfterPoint: ts(deletePoint)
        };

        // This weird mechanism is the only way to bypass mongod's attempt to fill in null
        // Timestamps.
        assert.writeOK(minValidColl.remove({}));
        assert.writeOK(minValidColl.update({}, {$set: injectedMinValidDoc}, {upsert: true}));
        assert.eq(minValidColl.findOne(),
                  injectedMinValidDoc,
                  "If the Timestamps differ, the server may be filling in the null timestamps");

        assert.writeOK(oplogTruncateAfterColl.remove({}));
        assert.writeOK(oplogTruncateAfterColl.update(
            {}, {$set: injectedOplogTruncateAfterPointDoc}, {upsert: true}));
        assert.eq(oplogTruncateAfterColl.findOne(),
                  injectedOplogTruncateAfterPointDoc,
                  "If the Timestamps differ, the server may be filling in the null timestamps");

        rst.stop(0);

        if (expectedState === 'FATAL') {
            try {
                rst.start(0, {waitForConnect: true}, true);
            } catch (e) {
            }
            rst.stop(0, undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
            return;
        } else {
            conn = rst.start(0, {waitForConnect: true}, true);
        }

        // Wait for the node to go to SECONDARY if it is able.
        assert.soon(
            () =>
                conn.adminCommand('serverStatus').metrics.repl.apply.attemptsToBecomeSecondary > 0,
            () => conn.adminCommand('serverStatus').metrics.repl.apply.attemptsToBecomeSecondary);

        var isMaster = conn.adminCommand('ismaster');
        switch (expectedState) {
            case 'SECONDARY':
                // Primary is also acceptable since once a node becomes secondary, it will try to
                // become primary if it is eligible and has enough votes (which this node does).
                // This is supposed to test that we reach secondary, not that we stay there.
                assert(isMaster.ismaster || isMaster.secondary,
                       'not PRIMARY or SECONDARY: ' + tojson(isMaster));
                break;

            case 'RECOVERING':
                assert(!isMaster.ismaster && !isMaster.secondary,
                       'not in RECOVERING: ' + tojson(isMaster));

                // Restart as a standalone node again so we can read from the collection.
                conn = rst.restart(0, {noReplSet: true});
                break;

            case 'FATAL':
                doassert("server startup didn't fail when it should have");
                break;

            default:
                doassert(`expectedState ${expectedState} is not supported`);
        }

        // Ensure the oplog has the entries it should have and none that it shouldn't.
        assert.eq(conn.getCollection('local.oplog.rs')
                      .find({ns: ns, op: 'i'})
                      .sort({$natural: 1})
                      .map((op) => op.o._id),
                  expectedApplied);

        // Ensure that all ops that should have been applied were.
        conn.setSlaveOk(true);
        assert.eq(conn.getCollection(ns).find().sort({_id: 1}).map((obj) => obj._id),
                  expectedApplied);
    }

    //
    // Normal 3.4 cases
    //

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: null,
        minValid: null,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: null,
        minValid: 2,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: null,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: 3,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: 4,
        begin: 3,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3],
        deletePoint: 4,
        begin: 3,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3, /*4,*/ 5, 6],
        collectionContents: [1, 2, 3],
        deletePoint: 4,
        begin: 3,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: 3,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3, 4, 5, 6],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: 3,
        minValid: 6,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3, 4, 5, 6],
    });

    //
    // These states should be impossible to get into.
    //

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3, 4],
        deletePoint: null,
        begin: 4,
        minValid: null,  // doesn't matter.
        expectedState: 'FATAL',
    });

    runTest({
        oplogEntries: [4, 5, 6],
        collectionContents: [1, 2],
        deletePoint: 2,
        begin: 3,
        minValid: null,  // doesn't matter.
        expectedState: 'FATAL',
    });

    runTest({
        oplogEntries: [4, 5, 6],
        collectionContents: [1, 2],
        deletePoint: null,
        begin: 3,
        minValid: null,  // doesn't matter.
        expectedState: 'FATAL',
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3, 4, 5],
        deletePoint: null,
        begin: 5,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3, 4, 5, 6],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3, 4, 5],
        deletePoint: null,
        begin: 5,
        minValid: null,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3, 4, 5, 6],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5],
        collectionContents: [1],
        deletePoint: 4,
        begin: 1,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    rst.stopSet();
})();
