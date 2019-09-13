// Test the populating lastOpVisible field of the ReplSetMetadata.
// First we do a writeConcern-free write and ensure that a local read will return the same
// lastOpVisible, and that majority read with afterOpTime of lastOpVisible will return it as well.
// We then confirm that a writeConcern majority write will be seen as the lastVisibleOp by a
// majority read.
// @tags: [requires_majority_read_concern]

load("jstests/replsets/rslib.js");

(function() {
    "use strict";

    var name = 'lastOpVisible';
    var replTest = new ReplSetTest(
        {name: name, nodes: 3, nodeOptions: {enableMajorityReadConcern: ''}, waitForKeys: true});

    if (!startSetIfSupportsReadMajority(replTest)) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        replTest.stopSet();
        return;
    }
    replTest.initiate();

    var primary = replTest.getPrimary();

    // Do an insert without writeConcern.
    var res = primary.getDB(name).runCommandWithMetadata({insert: name, documents: [{x: 1}]},
                                                         {"$replData": 1});
    assert.commandWorked(res.commandReply);
    var last_op_visible = res.metadata["$replData"].lastOpVisible;

    // A find should return the same lastVisibleOp.
    res = primary.getDB(name).runCommandWithMetadata({find: name, readConcern: {level: "local"}},
                                                     {"$replData": 1});
    assert.commandWorked(res.commandReply);
    assert.eq(last_op_visible, res.metadata["$replData"].lastOpVisible);

    // A majority readConcern with afterOpTime: lastOpVisible should also return the same
    // lastVisibleOp.
    res = primary.getDB(name).runCommandWithMetadata(
        {find: name, readConcern: {level: "majority", afterOpTime: last_op_visible}},
        {"$replData": 1});
    assert.commandWorked(res.commandReply);
    assert.eq(last_op_visible, res.metadata["$replData"].lastOpVisible);

    // Do an insert without writeConcern.
    res = primary.getDB(name).runCommandWithMetadata(
        {insert: name, documents: [{x: 1}], writeConcern: {w: "majority"}}, {"$replData": 1});
    assert.commandWorked(res.commandReply);
    last_op_visible = res.metadata["$replData"].lastOpVisible;

    // A majority readConcern should return the same lastVisibleOp.
    res = primary.getDB(name).runCommandWithMetadata({find: name, readConcern: {level: "majority"}},
                                                     {"$replData": 1});
    assert.commandWorked(res.commandReply);
    assert.eq(last_op_visible, res.metadata["$replData"].lastOpVisible);
    replTest.stopSet();
}());
