/**
 * Tests that a synthetic high-water-mark (HWM) token obeys the same semantics as a regular token.
 */
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
    load("jstests/libs/change_stream_util.js");  // For runCommandChangeStreamPassthroughAware.

    // Drop the test collections to assure a clean run.
    const collName = jsTestName();
    const otherCollName = "unrelated_" + collName;
    assertDropCollection(db, collName);
    assertDropCollection(db, otherCollName);

    // Helper function to ensure that the specified command is not modified by the passthroughs.
    function runExactCommand(db, cmdObj) {
        const doNotModifyInPassthroughs = true;
        return runCommandChangeStreamPassthroughAware(db, cmdObj, doNotModifyInPassthroughs);
    }

    let docId = 0;  // Tracks _id of documents inserted to ensure that we do not duplicate.

    // Open a stream on the test collection, before the collection has actually been created. Make
    // sure that this command is not modified in the passthroughs, since this behaviour is only
    // relevant for single-collection streams.
    let cmdResBeforeCollExists = assert.commandWorked(
        runExactCommand(db, {aggregate: collName, pipeline: [{$changeStream: {}}], cursor: {}}));

    // We should be able to retrieve a postBatchResumeToken (PBRT) even with no collection present.
    let csCursor = new DBCommandCursor(db, cmdResBeforeCollExists);
    let pbrtBeforeCollExists = csCursor.getResumeToken();
    assert.neq(undefined, pbrtBeforeCollExists);
    csCursor.close();

    // We can resumeAfter the token while the collection still does not exist.
    cmdResBeforeCollExists = assert.commandWorked(runExactCommand(db, {
        aggregate: collName,
        pipeline: [{$changeStream: {resumeAfter: pbrtBeforeCollExists}}],
        cursor: {}
    }));
    csCursor = new DBCommandCursor(db, cmdResBeforeCollExists);

    // But if the collection is created with a non-simple collation, the resumed stream invalidates.
    const testCollationCollection =
        assertCreateCollection(db, collName, {collation: {locale: "en_US", strength: 2}});
    assert.commandWorked(testCollationCollection.insert({_id: "insert_one"}));
    assert.commandWorked(testCollationCollection.insert({_id: "INSERT_TWO"}));
    assert.soon(() => csCursor.hasNext());
    const invalidate = csCursor.next();
    assert.eq(invalidate.operationType, "invalidate");  // We don't see either insert.
    csCursor.close();

    // We can resume from the pre-creation high water mark if we specify an explicit collation...
    let cmdResResumeFromBeforeCollCreated = assert.commandWorked(runExactCommand(db, {
        aggregate: collName,
        pipeline: [
            {$changeStream: {resumeAfter: pbrtBeforeCollExists}},
            {
              $match:
                  {$or: [{"fullDocument._id": "INSERT_ONE"}, {"fullDocument._id": "INSERT_TWO"}]}
            }
        ],
        collation: {locale: "simple"},
        cursor: {}
    }));

    // This time the stream does not invalidate. We override the default case-insensitive collation
    // with the explicit simple collation we specified, so we match INSERT_TWO but not INSERT_ONE.
    csCursor = new DBCommandCursor(db, cmdResResumeFromBeforeCollCreated);
    assert.soon(() => csCursor.hasNext());
    assert.docEq(csCursor.next().fullDocument, {_id: "INSERT_TWO"});
    csCursor.close();

    // Now open a change stream with batchSize:0 in order to produce a new high water mark.
    const cmdResCollWithCollation = assert.commandWorked(runExactCommand(db, {
        aggregate: collName,
        pipeline: [
            {$changeStream: {}},
        ],
        cursor: {batchSize: 0}
    }));
    csCursor = new DBCommandCursor(db, cmdResCollWithCollation);
    const hwmFromCollWithCollation = csCursor.getResumeToken();
    assert.neq(undefined, hwmFromCollWithCollation);
    csCursor.close();

    // Insert two more documents into the collection for testing purposes.
    assert.commandWorked(testCollationCollection.insert({_id: "insert_three"}));
    assert.commandWorked(testCollationCollection.insert({_id: "INSERT_FOUR"}));

    // We can resume the stream on the collection using the HWM...
    const cmdResResumeWithCollation = assert.commandWorked(runExactCommand(db, {
        aggregate: collName,
        pipeline: [
            {$changeStream: {resumeAfter: hwmFromCollWithCollation}},
            {$match: {"fullDocument._id": "insert_four"}}
        ],
        cursor: {}
    }));
    csCursor = new DBCommandCursor(db, cmdResResumeWithCollation);

    // ... and we inherit the collection's case-insensitive collation, matching {_id:"insert_four"}.
    assert.soon(() => csCursor.hasNext());
    assert.docEq(csCursor.next().fullDocument, {_id: "INSERT_FOUR"});
    csCursor.close();

    // Drop the collection and obtain a new pre-creation high water mark. We will use this later.
    assertDropCollection(db, collName);
    cmdResBeforeCollExists = assert.commandWorked(
        runExactCommand(db, {aggregate: collName, pipeline: [{$changeStream: {}}], cursor: {}}));
    csCursor = new DBCommandCursor(db, cmdResBeforeCollExists);
    pbrtBeforeCollExists = csCursor.getResumeToken();
    assert.neq(undefined, pbrtBeforeCollExists);
    csCursor.close();

    // Now create each of the test collections with the default simple collation.
    const testCollection = assertCreateCollection(db, collName);
    const otherCollection = assertCreateCollection(db, otherCollName);
    const adminDB = db.getSiblingDB("admin");

    // Open a stream on the test collection. Write one document to the test collection and one to
    // the unrelated collection, in order to push the postBatchResumeToken (PBRT) past the last
    // related event.
    csCursor = testCollection.watch();
    assert.commandWorked(testCollection.insert({_id: docId++}));
    assert.commandWorked(otherCollection.insert({}));

    // Consume all events. The PBRT of the batch should be greater than the last event, which
    // guarantees that it is a synthetic high-water-mark token.
    let relatedEvent = null;
    let hwmToken = null;
    assert.soon(() => {
        if (csCursor.hasNext()) {
            relatedEvent = csCursor.next();
        }
        assert.eq(csCursor.objsLeftInBatch(), 0);
        hwmToken = csCursor.getResumeToken();
        assert.neq(undefined, hwmToken);
        return relatedEvent && bsonWoCompare(hwmToken, relatedEvent._id) > 0;
    });
    csCursor.close();

    // Now write some further documents to the collection before attempting to resume.
    for (let i = 0; i < 5; ++i) {
        assert.commandWorked(testCollection.insert({_id: docId++}));
    }

    // We can resumeAfter the high water mark. We only see the latest 5 documents.
    csCursor = testCollection.watch([], {resumeAfter: hwmToken});
    assert.soon(() => {
        if (csCursor.hasNext()) {
            relatedEvent = csCursor.next();
            assert.gt(bsonWoCompare(relatedEvent._id, hwmToken), 0);
            // We never see the first document, whose _id was 0.
            assert.gt(relatedEvent.fullDocument._id, 0);
        }
        // The _id of the last document inserted is (docId-1).
        return relatedEvent.fullDocument._id === (docId - 1);
    });
    csCursor.close();

    // Now resumeAfter the token that was generated before the collection was created...
    cmdResResumeFromBeforeCollCreated = assert.commandWorked(runExactCommand(db, {
        aggregate: collName,
        pipeline: [{$changeStream: {resumeAfter: pbrtBeforeCollExists}}],
        cursor: {}
    }));
    // ... and confirm that we see all the events that have occurred since then.
    csCursor = new DBCommandCursor(db, cmdResResumeFromBeforeCollCreated);
    let docCount = 0;
    assert.soon(() => {
        if (csCursor.hasNext()) {
            relatedEvent = csCursor.next();
            assert.eq(relatedEvent.fullDocument._id, docCount++);
        }
        return docCount === docId;
    });

    // Despite the fact that we just resumed from a token which was generated before the collection
    // existed and had no UUID, all subsequent HWMs should now have UUIDs. To test this, we first
    // get the current resume token, then write a document to the unrelated collection. We then wait
    // until the PBRT advances, which means that we now have a new HWM token.
    let hwmPostCreation = csCursor.getResumeToken();
    assert.commandWorked(otherCollection.insert({}));
    assert.soon(() => {
        assert(!csCursor.hasNext());
        return bsonWoCompare(csCursor.getResumeToken(), hwmPostCreation) > 0;
    });
    hwmPostCreation = csCursor.getResumeToken();
    csCursor.close();

    // Because this HWM has a UUID, we cannot resume from the token if the collection is dropped...
    assertDropCollection(db, collName);
    assert.commandFailedWithCode(runExactCommand(db, {
                                     aggregate: collName,
                                     pipeline: [{$changeStream: {resumeAfter: hwmPostCreation}}],
                                     cursor: {}
                                 }),
                                 ErrorCodes.InvalidResumeToken);
    // ... or if the collection is recreated with a different UUID...
    assertCreateCollection(db, collName);
    assert.commandFailedWithCode(runExactCommand(db, {
                                     aggregate: collName,
                                     pipeline: [{$changeStream: {resumeAfter: hwmPostCreation}}],
                                     cursor: {}
                                 }),
                                 ErrorCodes.InvalidResumeToken);
    // ... unless we specify an explicit collation.
    assert.commandWorked(runExactCommand(db, {
        aggregate: collName,
        pipeline: [{$changeStream: {resumeAfter: hwmPostCreation}}],
        collation: {locale: "simple"},
        cursor: {}
    }));

    // But even after the collection is recreated, we can still resume from the pre-creation HWM...
    cmdResResumeFromBeforeCollCreated = assert.commandWorked(runExactCommand(db, {
        aggregate: collName,
        pipeline: [{$changeStream: {resumeAfter: pbrtBeforeCollExists}}],
        cursor: {}
    }));
    // ...and can still see all the events from the collection's original incarnation...
    csCursor = new DBCommandCursor(db, cmdResResumeFromBeforeCollCreated);
    docCount = 0;
    assert.soon(() => {
        if (csCursor.hasNext()) {
            relatedEvent = csCursor.next();
            assert.eq(relatedEvent.fullDocument._id, docCount++);
        }
        return docCount === docId;
    });
    // ... this time followed by an invalidate, as the collection is dropped.
    assert.soon(() => {
        return csCursor.hasNext() && csCursor.next().operationType === "invalidate";
    });
    csCursor.close();
})();