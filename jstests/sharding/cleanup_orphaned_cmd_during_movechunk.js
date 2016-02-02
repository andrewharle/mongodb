//
// Tests cleanupOrphaned concurrent with moveChunk.
// Inserts orphan documents to the donor and recipient shards during the moveChunk and 
// verifies that cleanupOrphaned removes orphans.
//

load('./jstests/libs/chunk_manipulation_util.js');
load('./jstests/libs/cleanup_orphaned_util.js');

(function() { 
"use strict";

var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.
var st = new ShardingTest({shards: 2, mongos: 1, 
                          other: {separateConfig: true, shardOptions: {verbose: 0}}});

var mongos = st.s0,
    admin = mongos.getDB('admin'),
    shards = mongos.getCollection('config.shards').find().toArray(),
    dbName = 'foo',
    ns = dbName + '.bar',
    coll = mongos.getCollection(ns),
    donor = st.shard0,
    recipient = st.shard1,
    donorColl = donor.getCollection(ns),
    recipientColl = st.shard1.getCollection(ns);

// Three chunks of 10 documents each, with ids -20, -18, -16, ..., 38.
// Donor:     [minKey, 0) [0, 20)
// Recipient:                [20, maxKey)
assert.commandWorked( admin.runCommand({enableSharding: dbName}) );
printjson( admin.runCommand({movePrimary: dbName, to: shards[0]._id}) );
assert.commandWorked( admin.runCommand({shardCollection: ns, key: {_id: 1}}) );
assert.commandWorked( admin.runCommand({split: ns, middle: {_id: 0}}) );
assert.commandWorked( admin.runCommand({split: ns, middle: {_id: 20}}) );
assert.commandWorked( admin.runCommand({moveChunk: ns,
                         find: {_id: 20},
                         to: shards[1]._id,
                         _waitForDelete: true}) );

jsTest.log('Inserting 40 docs into shard 0....');
for (var i = -20; i < 20; i += 2) coll.insert({_id: i});
assert.eq(null, coll.getDB().getLastError());
assert.eq(20, donorColl.count());

jsTest.log('Inserting 25 docs into shard 1....');
for (i = 20; i < 40; i += 2) coll.insert({_id: i});
assert.eq(null, coll.getDB().getLastError());
assert.eq(10, recipientColl.count());

//
// Start a moveChunk in the background. Move chunk [0, 20), which has 10 docs,
// from shard 0 to shard 1. Pause it at some points in the donor's and
// recipient's work flows, and test cleanupOrphaned on shard 0 and shard 1.
//

jsTest.log('setting failpoint startedMoveChunk');
pauseMoveChunkAtStep(donor, moveChunkStepNames.startedMoveChunk);
pauseMigrateAtStep(recipient, migrateStepNames.cloned);
var joinMoveChunk = moveChunkParallel(
    staticMongod,
    st.s0.host,
    {_id: 0},
    null,
    coll.getFullName(),
    shards[1]._id);

waitForMoveChunkStep(donor, moveChunkStepNames.startedMoveChunk);
waitForMigrateStep(recipient, migrateStepNames.cloned);
// Recipient has run _recvChunkStart and begun its migration thread; docs have
// been cloned and chunk [0, 20) is noted as 'pending' on recipient.

// Donor:     [minKey, 0) [0, 20)
// Recipient (pending):   [0, 20)
// Recipient:                [20, maxKey)

// Create orphans. I'll show an orphaned doc on donor with _id 26 like {26}:
//
// Donor:     [minKey, 0) [0, 20) {26}
// Recipient (pending):   [0, 20)
// Recipient:  {-1}          [20, maxKey)
donorColl.insert([{_id: 26}]);
assert.eq(null, donorColl.getDB().getLastError());
assert.eq(21, donorColl.count());
recipientColl.insert([{_id: -1}]);
assert.eq(null, recipientColl.getDB().getLastError());
assert.eq(21, recipientColl.count());

cleanupOrphaned(donor, ns, 2);
assert.eq(20, donorColl.count());
cleanupOrphaned(recipient, ns, 2);
assert.eq(20, recipientColl.count());

jsTest.log('Inserting document on donor side');
// Inserted a new document (not an orphan) with id 19, which belongs in the
// [0, 20) chunk.
donorColl.insert({_id: 19});
assert.eq(null, coll.getDB().getLastError());
assert.eq(21, donorColl.count());

// Recipient transfers this modification.
jsTest.log('Let migrate proceed to transferredMods');
pauseMigrateAtStep(recipient, migrateStepNames.transferredMods);
unpauseMigrateAtStep(recipient, migrateStepNames.cloned);
waitForMigrateStep(recipient, migrateStepNames.transferredMods);
jsTest.log('Done letting migrate proceed to transferredMods');

assert.eq(
    21, recipientColl.count(),
    "Recipient didn't transfer inserted document.");

cleanupOrphaned(donor, ns, 2);
assert.eq(21, donorColl.count());
cleanupOrphaned(recipient, ns, 2);
assert.eq(21, recipientColl.count());

// Create orphans.
donorColl.insert([{_id: 26}]);
assert.eq(null, donorColl.getDB().getLastError());
assert.eq(22, donorColl.count());
recipientColl.insert([{_id: -1}]);
assert.eq(null, recipientColl.getDB().getLastError());
assert.eq(22, recipientColl.count());

cleanupOrphaned(donor, ns, 2);
assert.eq(21, donorColl.count());
cleanupOrphaned(recipient, ns, 2);
assert.eq(21, recipientColl.count());

// Recipient has been waiting for donor to call _recvChunkCommit.
pauseMoveChunkAtStep(donor, moveChunkStepNames.committed);
unpauseMoveChunkAtStep(donor, moveChunkStepNames.startedMoveChunk);
unpauseMigrateAtStep(recipient, migrateStepNames.transferredMods);
proceedToMigrateStep(recipient, migrateStepNames.done);

// Create orphans.
donorColl.insert([{_id: 26}]);
assert.eq(null, donorColl.getDB().getLastError());
assert.eq(22, donorColl.count());
recipientColl.insert([{_id: -1}]);
assert.eq(null, recipientColl.getDB().getLastError());
assert.eq(22, recipientColl.count());

// cleanupOrphaned should still fail on donor, but should work on the recipient
cleanupOrphaned(donor, ns, 2);
assert.eq(10, donorColl.count());
cleanupOrphaned(recipient, ns, 2);
assert.eq(21, recipientColl.count());

// Let migration thread complete.
unpauseMigrateAtStep(recipient, migrateStepNames.done);
unpauseMoveChunkAtStep(donor, moveChunkStepNames.committed);
joinMoveChunk();

// Donor has finished post-move delete.
cleanupOrphaned(donor, ns, 2); // this is necessary for the count to not be 11
assert.eq(10, donorColl.count());
assert.eq(21, recipientColl.count());
assert.eq(31, coll.count());

jsTest.log('DONE!');
st.stop();

})()
