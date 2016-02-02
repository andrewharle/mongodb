//
//
// Tests cleanupOrphaned concurrent with moveChunk with a hashed shard key.
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
    coll = mongos.getCollection(ns);

assert.commandWorked( admin.runCommand({enableSharding: dbName}) );
printjson(admin.runCommand({movePrimary: dbName, to: shards[0]._id}));
assert.commandWorked( admin.runCommand({shardCollection: ns, key: {key: 'hashed'}}) );

// Makes four chunks by default, two on each shard.
var chunks = st.config.chunks.find().sort({min: 1}).toArray();
assert.eq(4, chunks.length);

var chunkWithDoc = chunks[1];
print('Trying to make doc that hashes to this chunk: '
      + tojson(chunkWithDoc));

var found = false;
for (var i = 0; i < 10000; i++) {
    var doc = {key: ObjectId()},
        hash = mongos.adminCommand({_hashBSONElement: doc.key}).out;

    print('doc.key ' + doc.key + ' hashes to ' + hash);

    if (mongos.getCollection('config.chunks').findOne({
        _id: chunkWithDoc._id,
        'min.key': {$lte: hash},
        'max.key': {$gt: hash}
    })) {
        found = true;
        break;
    }
}

assert(found, "Couldn't make doc that belongs to chunk 1.");
print('Doc: ' + tojson(doc));
coll.insert(doc);
assert.eq(null, coll.getDB().getLastError());

//
// Start a moveChunk in the background from shard 0 to shard 1. Pause it at
// some points in the donor's and recipient's work flows, and test
// cleanupOrphaned.
//

var donor, recip;
if (chunkWithDoc.shard == st.shard0.shardName) {
    donor = st.shard0;
    recip = st.shard1;
} else {
    recip = st.shard0;
    donor = st.shard1;
}

jsTest.log('setting failpoint startedMoveChunk');
pauseMoveChunkAtStep(donor, moveChunkStepNames.startedMoveChunk);
pauseMigrateAtStep(recip, migrateStepNames.cloned);

var joinMoveChunk = moveChunkParallel(
    staticMongod,
    st.s0.host,
    null,
    [chunkWithDoc.min, chunkWithDoc.max],  // bounds
    coll.getFullName(),
    recip.shardName);

waitForMoveChunkStep(donor, moveChunkStepNames.startedMoveChunk);
waitForMigrateStep(recip, migrateStepNames.cloned);
proceedToMigrateStep(recip, migrateStepNames.transferredMods);
// recipient has run _recvChunkStart and begun its migration thread;
// 'doc' has been cloned and chunkWithDoc is noted as 'pending' on recipient.

var donorColl = donor.getCollection(ns),
    recipColl = recip.getCollection(ns);

assert.eq(1, donorColl.count());
assert.eq(1, recipColl.count());

// cleanupOrphaned should go through two iterations, since the default chunk
// setup leaves two unowned ranges on each shard.
cleanupOrphaned(donor, ns, 2);
cleanupOrphaned(recip, ns, 2);
assert.eq(1, donorColl.count());
assert.eq(1, recipColl.count());

// recip has been waiting for donor to call _recvChunkCommit.
pauseMoveChunkAtStep(donor, moveChunkStepNames.committed);
unpauseMoveChunkAtStep(donor, moveChunkStepNames.startedMoveChunk);
unpauseMigrateAtStep(recip, migrateStepNames.transferredMods);
proceedToMigrateStep(recip, migrateStepNames.done);

// cleanupOrphaned removes migrated data from donor. The donor would
// otherwise clean them up itself, in the post-move delete phase.
cleanupOrphaned(donor, ns, 2);
assert.eq(0, donorColl.count());
cleanupOrphaned(recip, ns, 2);
assert.eq(1, recipColl.count());

// Let migration thread complete.
unpauseMigrateAtStep(recip, migrateStepNames.done);
unpauseMoveChunkAtStep(donor, moveChunkStepNames.committed);
joinMoveChunk();

// donor has finished post-move delete.
assert.eq(0, donorColl.count());
assert.eq(1, recipColl.count());
assert.eq(1, coll.count());

jsTest.log('DONE!');
st.stop();

})()
