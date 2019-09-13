// Tests that it is illegal to write to system collections within a transaction.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const session = db.getMongo().startSession({causalConsistency: false});

    // Use a custom database, to avoid conflict with other tests that use the system.js collection.
    const testDB = session.getDatabase("no_writes_system_collections_in_txn");
    assert.commandWorked(testDB.dropDatabase());
    const systemColl = testDB.getCollection("system.js");

    // Ensure that a collection exists with at least one document.
    assert.commandWorked(systemColl.insert({name: 0}, {writeConcern: {w: "majority"}}));

    session.startTransaction({readConcern: {level: "snapshot"}});
    let error = assert.throws(() => systemColl.findAndModify({query: {}, update: {}}));
    assert.commandFailedWithCode(error, 50781);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.startTransaction({readConcern: {level: "snapshot"}});
    error = assert.throws(() => systemColl.findAndModify({query: {}, remove: true}));
    assert.commandFailedWithCode(error, 50781);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(systemColl.insert({name: "new"}), 50791);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(systemColl.update({name: 0}, {$set: {name: "jungsoo"}}), 50791);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(
        systemColl.update({name: "nonexistent"}, {$set: {name: "jungsoo"}}, {upsert: true}), 50791);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(systemColl.remove({name: 0}), 50791);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    assert.commandWorked(systemColl.remove({_id: {$exists: true}}));
    assert.eq(systemColl.find().itcount(), 0);
}());
