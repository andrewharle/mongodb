// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

/**
 * Tests that using the $set update modifier to change only the type of a field will actually update
 * the document, including any relevant indices.
 */
(function() {
    "use strict";

    var coll = db.set_type_change;
    coll.drop();
    assert.commandWorked(coll.ensureIndex({a: 1}));

    assert.writeOK(coll.insert({a: 2}));

    var newVal = new NumberLong(2);
    var res = coll.update({}, {$set: {a: newVal}});
    assert.eq(res.nMatched, 1);
    if (coll.getMongo().writeMode() == "commands")
        assert.eq(res.nModified, 1);

    // Make sure it actually changed the type.
    var updated = coll.findOne();
    assert(updated.a instanceof NumberLong, "$set did not update type of value: " + updated.a);
})();
