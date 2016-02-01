// This is a regression test for SERVER-21545.
//
// Tests that a collMod with a bad specification does not cause any changes, and does not crash the
// server.
(function() {
    "use strict";

    var collName = "collModBadSpec";
    var coll = db.getCollection(collName);

    coll.drop();
    assert.commandWorked(db.createCollection(collName));

    // Get the original collection options for the collection.
    var originalResult = db[collName].exists();

    // Issue an invalid command.
    assert.commandFailed(coll.runCommand("collMod", {usePowerOf2Sizes: true, unknownField: "x"}));

    // Make sure the options are unchanged.
    var newResult = db[collName].exists();
    assert.eq(originalResult, newResult);
})();
