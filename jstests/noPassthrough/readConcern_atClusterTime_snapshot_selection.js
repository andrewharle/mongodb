// Test that 'atClusterTime' is used to select the snapshot for reads. We wait for 'atClusterTime'
// to be majority committed. If 'atClusterTime' is older than the oldest available snapshot, the
// error code SnapshotTooOld is returned.
//
// @tags: [uses_transactions, requires_majority_read_concern]
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");  // For stopServerReplication.

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 3, settings: {chainingAllowed: false}});
    rst.startSet();
    rst.initiate();

    const primarySession =
        rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
    const primaryDB = primarySession.getDatabase(dbName);

    const secondaryConns = rst.getSecondaries();
    const secondaryConn0 = secondaryConns[0];
    const secondaryConn1 = secondaryConns[1];
    const secondarySession =
        secondaryConn0.getDB(dbName).getMongo().startSession({causalConsistency: false});
    const secondaryDB0 = secondarySession.getDatabase(dbName);

    // Create the collection and insert one document. Get the op time of the write.
    let res = assert.commandWorked(primaryDB.runCommand(
        {insert: collName, documents: [{_id: "before"}], writeConcern: {w: "majority"}}));
    let clusterTimePrimaryBefore;

    // Wait for the majority commit point on 'secondaryDB0' to include the {_id: "before"} write.
    assert.soonNoExcept(function() {
        // Without a consistent stream of writes, secondary majority reads are not guaranteed
        // to complete, since the commit point being stale is not sufficient to establish a sync
        // source.
        // TODO (SERVER-33248): Remove this write and increase the maxTimeMS on the read.
        res = assert.commandWorked(primaryDB.runCommand(
            {insert: "otherColl", documents: [{a: 1}], writeConcern: {w: "majority"}}));
        assert(res.hasOwnProperty("opTime"), tojson(res));
        assert(res.opTime.hasOwnProperty("ts"), tojson(res));
        clusterTimePrimaryBefore = res.opTime.ts;

        return assert
                   .commandWorked(secondaryDB0.runCommand(
                       {find: collName, readConcern: {level: "majority"}, maxTimeMS: 10000}))
                   .cursor.firstBatch.length === 1;
    });

    // Stop replication on both secondaries.
    stopServerReplication(secondaryConn0);
    stopServerReplication(secondaryConn1);

    // Perform write and get the op time of the write.
    res =
        assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{_id: "after"}]}));
    assert(res.hasOwnProperty("opTime"), tojson(res));
    assert(res.opTime.hasOwnProperty("ts"), tojson(res));
    let clusterTimeAfter = res.opTime.ts;

    // A read on the primary at the old cluster time should not include the write.
    primarySession.startTransaction(
        {readConcern: {level: "snapshot", atClusterTime: clusterTimePrimaryBefore}});
    res = assert.commandWorked(primaryDB.runCommand({find: collName}));
    primarySession.commitTransaction();
    assert.eq(res.cursor.firstBatch.length, 1, printjson(res));
    assert.eq(res.cursor.firstBatch[0]._id, "before", printjson(res));

    // A read on the primary at the new cluster time should time out waiting for the cluster time to
    // be majority committed.
    primarySession.startTransaction(
        {readConcern: {level: "snapshot", atClusterTime: clusterTimeAfter}});
    assert.commandFailedWithCode(primaryDB.runCommand({find: collName, maxTimeMS: 1000}),
                                 ErrorCodes.MaxTimeMSExpired);
    primarySession.abortTransaction();

    // Restart replication on one of the secondaries.
    restartServerReplication(secondaryConn1);

    // A read on the primary at the new cluster time now succeeds.
    primarySession.startTransaction(
        {readConcern: {level: "snapshot", atClusterTime: clusterTimeAfter}});
    res = assert.commandWorked(primaryDB.runCommand({find: collName}));
    primarySession.commitTransaction();
    assert.eq(res.cursor.firstBatch.length, 2, printjson(res));

    // A read on the lagged secondary at its view of the majority cluster time should not include
    // the write.
    const clusterTimeSecondaryBefore = rst.getReadConcernMajorityOpTimeOrThrow(secondaryConn0).ts;
    secondarySession.startTransaction(
        {readConcern: {level: "snapshot", atClusterTime: clusterTimeSecondaryBefore}});
    res = assert.commandWorked(secondaryDB0.runCommand({find: collName}));
    secondarySession.commitTransaction();
    assert.eq(res.cursor.firstBatch.length, 1, printjson(res));
    assert.eq(res.cursor.firstBatch[0]._id, "before", printjson(res));

    // A read on the lagged secondary at the new cluster time should time out waiting for an op at
    // that cluster time.
    secondarySession.startTransaction(
        {readConcern: {level: "snapshot", atClusterTime: clusterTimeAfter}});
    assert.commandFailedWithCode(secondaryDB0.runCommand({find: collName, maxTimeMS: 1000}),
                                 ErrorCodes.MaxTimeMSExpired);
    secondarySession.abortTransaction();

    // Restart replication on the lagged secondary.
    restartServerReplication(secondaryConn0);

    // A read on the secondary at the new cluster time now succeeds.
    secondarySession.startTransaction(
        {readConcern: {level: "snapshot", atClusterTime: clusterTimeAfter}});
    res = assert.commandWorked(secondaryDB0.runCommand({find: collName}));
    secondarySession.commitTransaction();
    assert.eq(res.cursor.firstBatch.length, 2, printjson(res));

    // A read at a time that is too old fails.
    primarySession.startTransaction(
        {readConcern: {level: "snapshot", atClusterTime: Timestamp(1, 1)}});
    assert.commandFailedWithCode(primaryDB.runCommand({find: collName}), ErrorCodes.SnapshotTooOld);
    primarySession.abortTransaction();

    rst.stopSet();
}());
