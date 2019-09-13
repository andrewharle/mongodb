/**
 * Test that a rollback of collMod altering TTL will cause the node to go into a FATAL state
 *
 * If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
 * not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
 * scenario, none of the members will have any data, and upon restart will each look for a member to
 * initial sync from, so no primary will be elected. This test induces such a scenario, so cannot be
 * run on ephemeral storage engines.

 * This only tests rollback via refetch behavior and thus only runs on mmapv1.
 * @tags: [requires_persistence, requires_mmapv1]
 */

// Sets up a replica set and grabs things for later.
var name = "rollback_collMod_fatal";
var replTest = new ReplSetTest({name: name, nodes: 3});
var nodes = replTest.nodeList();
var conns = replTest.startSet();
replTest.initiate({
    "_id": name,
    "members": [
        {"_id": 0, "host": nodes[0], priority: 3},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2], arbiterOnly: true}
    ]
});
var a_conn = conns[0];
var b_conn = conns[1];
var AID = replTest.getNodeId(a_conn);
var BID = replTest.getNodeId(b_conn);

replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);

// Gets master and does an initial write.
var master = replTest.getPrimary();
assert(master === conns[0], "conns[0] assumed to be master");
assert(a_conn.host === master.host, "a_conn assumed to be master");
var options = {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}, upsert: true};
a_conn.getDB(name).foo.ensureIndex({x: 1}, {expireAfterSeconds: 3600});
assert.writeOK(a_conn.getDB(name).foo.insert({x: 1}, options));

// Shuts down the master.
replTest.stop(AID);

// Does a collMod altering TTL which should cause FATAL when rolled back.
master = replTest.getPrimary();
assert(b_conn.host === master.host, "b_conn assumed to be master");
assert.commandWorked(b_conn.getDB(name).runCommand(
    {collMod: "foo", index: {keyPattern: {x: 1}, expireAfterSeconds: 10}}));

// Shuts down B and brings back the original master.
replTest.stop(BID);
replTest.restart(AID);
master = replTest.getPrimary();
assert(a_conn.host === master.host, "a_conn assumed to be master");

// Does a write so that B will have to roll back.
options = {
    writeConcern: {w: 1, wtimeout: ReplSetTest.kDefaultTimeoutMS},
    upsert: true
};
assert.writeOK(a_conn.getDB(name).foo.insert({x: 2}, options));

// Restarts B, which should attempt rollback but then fassert.
clearRawMongoProgramOutput();
try {
    b_conn = replTest.start(BID, {waitForConnect: true}, true /*restart*/);
} catch (e) {
    // We swallow the exception from ReplSetTest#start() because it means that the server
    // fassert()'d before the mongo shell could connect to it.
}
// Wait for node B to fassert
assert.soon(function() {
    try {
        b_conn.getDB("local").runCommand({ping: 1});
    } catch (e) {
        return true;
    }
    return false;
}, "Node did not fassert", 60 * 1000);

replTest.stop(BID, undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});

assert(rawMongoProgramOutput().match("Cannot roll back a collMod command"), "B failed to fassert");

replTest.stopSet();
