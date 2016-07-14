/**
 * @tags: [requires_journaling]
 *
 * Tests that writeConcern 'majority' writes succeed and are visible in a replica set that has one
 * data-bearing node and two arbiters.
 */

(function() {
    "use strict";

    // Set up a set and grab things for later.
    var name = "read_majority_two_arbs";
    var replTest =
        new ReplSetTest({name: name, nodes: 3, nodeOptions: {enableMajorityReadConcern: ''}});
    var nodes = replTest.nodeList();

    try {
        replTest.startSet();
    } catch (e) {
        var conn = MongoRunner.runMongod();
        if (!conn.getDB('admin').serverStatus().storageEngine.supportsCommittedReads) {
            jsTest.log("skipping test since storage engine doesn't support committed reads");
            MongoRunner.stopMongod(conn);
            return;
        }
        throw e;
    }

    replTest.initiate({
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1], arbiterOnly: true},
            {"_id": 2, "host": nodes[2], arbiterOnly: true}
        ]
    });

    var primary = replTest.getPrimary();
    var db = primary.getDB(name);
    var t = db[name];

    function doRead(readConcern) {
        var res = assert.commandWorked(t.runCommand('find', readConcern));
        var docs = (new DBCommandCursor(db.getMongo(), res)).toArray();
        assert.gt(docs.length, 0, "no docs returned!");
        return docs[0].state;
    }

    function doDirtyRead() {
        return doRead({"readConcern": {"level": "local"}});
    }

    function doCommittedRead() {
        return doRead({"readConcern": {"level": "majority"}});
    }

    jsTest.log("doing write");
    assert.writeOK(
        t.save({_id: 1, state: 0}, {writeConcern: {w: "majority", wtimeout: 10 * 1000}}));
    jsTest.log("doing read");
    assert.eq(doDirtyRead(), 0);
    jsTest.log("doing committed read");
    assert.eq(doCommittedRead(), 0);
    jsTest.log("stopping replTest; test completed successfully");
    replTest.stopSet();
}());
