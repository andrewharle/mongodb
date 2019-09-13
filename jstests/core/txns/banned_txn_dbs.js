// Tests that reads and writes to the config, admin, and local databases are forbidden within
// transactions.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const session = db.getMongo().startSession({causalConsistency: false});
    const collName = "banned_txn_dbs";

    function runTest(sessionDB) {
        jsTest.log("Testing database " + sessionDB.getName());

        let sessionColl = sessionDB[collName];
        sessionColl.drop({writeConcern: {w: "majority"}});
        assert.commandWorked(sessionDB.createCollection(collName, {writeConcern: {w: "majority"}}));

        jsTest.log("Testing read commands are forbidden.");
        session.startTransaction();
        let error = assert.throws(() => sessionColl.find().itcount());
        assert.commandFailedWithCode(error, ErrorCodes.OperationNotSupportedInTransaction);
        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);

        jsTest.log("Testing write commands are forbidden.");
        session.startTransaction();
        assert.commandFailedWithCode(sessionColl.insert({}),
                                     ErrorCodes.OperationNotSupportedInTransaction);
        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
    }

    runTest(session.getDatabase("config"));
    runTest(session.getDatabase("admin"));
    runTest(session.getDatabase("local"));

    session.endSession();
}());
