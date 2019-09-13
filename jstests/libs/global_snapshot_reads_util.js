/**
 * Tests invalid getMore attempts against an established global snapshot cursor on mongos. The
 * cursor should still be valid and usable after each failed attempt.
 */
function verifyInvalidGetMoreAttempts(mainDb, sessionDb, collName, cursorId, txnNumber) {
    // Reject getMores without a session.
    assert.commandFailedWithCode(
        mainDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}), 50800);

    // Subsequent getMore requests without the same session id are rejected. The cursor should
    // still be valid and usable after this failed attempt.
    assert.commandFailedWithCode(mainDb.runCommand({
        getMore: cursorId,
        collection: collName,
        batchSize: 1,
        txnNumber: NumberLong(txnNumber),
        lsid: {id: UUID()}
    }),
                                 50801);

    // Reject getMores without without txnNumber.
    assert.commandFailedWithCode(
        sessionDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}), 50803);

    // Reject getMores without without same txnNumber.
    assert.commandFailedWithCode(
        sessionDb.runCommand(
            {getMore: cursorId, collection: collName, batchSize: 1, txnNumber: NumberLong(50)}),
        50804);
}
