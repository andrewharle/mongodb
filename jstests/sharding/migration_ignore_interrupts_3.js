// If a donor aborts a migration to a recipient, the recipient does not realize the migration has
// been aborted, and the donor moves on to a new migration, the original recipient will then fail to
// clone documents from the donor.
//
// Note: don't use coll1 in this test after a coll1 migration is interrupted -- the distlock isn't
// released promptly when interrupted.

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
    "use strict";

    var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

    var st = new ShardingTest({shards: 3});

    var mongos = st.s0, admin = mongos.getDB('admin'), dbName = "testDB", ns1 = dbName + ".foo",
        ns2 = dbName + ".bar", coll1 = mongos.getCollection(ns1), coll2 = mongos.getCollection(ns2),
        shard0 = st.shard0, shard1 = st.shard1, shard2 = st.shard2,
        shard0Coll1 = shard0.getCollection(ns1), shard1Coll1 = shard1.getCollection(ns1),
        shard2Coll1 = shard2.getCollection(ns1), shard0Coll2 = shard0.getCollection(ns2),
        shard1Coll2 = shard1.getCollection(ns2), shard2Coll2 = shard2.getCollection(ns2);

    assert.commandWorked(admin.runCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    assert.commandWorked(admin.runCommand({shardCollection: ns1, key: {a: 1}}));
    assert.writeOK(coll1.insert({a: 0}));
    assert.eq(1, shard0Coll1.find().itcount());
    assert.eq(0, shard1Coll1.find().itcount());
    assert.eq(0, shard2Coll1.find().itcount());
    assert.eq(1, coll1.find().itcount());

    assert.commandWorked(admin.runCommand({shardCollection: ns2, key: {a: 1}}));
    assert.writeOK(coll2.insert({a: 0}));
    assert.eq(1, shard0Coll2.find().itcount());
    assert.eq(0, shard1Coll2.find().itcount());
    assert.eq(0, shard2Coll2.find().itcount());
    assert.eq(1, coll2.find().itcount());

    // Shard0:
    //      coll1:     [-inf, +inf)
    //      coll2:     [-inf, +inf)
    // Shard1:
    // Shard2:

    jsTest.log("Set up complete, now proceeding to test that migration interruption fails.");

    // Start coll1 migration to shard1: pause recipient after delete step, donor before interrupt
    // check.
    pauseMigrateAtStep(shard1, migrateStepNames.deletedPriorDataInRange);
    pauseMoveChunkAtStep(shard0, moveChunkStepNames.startedMoveChunk);
    var joinMoveChunk = moveChunkParallel(
        staticMongod, st.s0.host, {a: 0}, null, coll1.getFullName(), st.shard1.shardName);
    waitForMigrateStep(shard1, migrateStepNames.deletedPriorDataInRange);

    // Abort migration on donor side, recipient is unaware.
    let inProgressOps = admin.aggregate([{$currentOp: {'allUsers': true}}]);
    var abortedMigration = false;
    let inProgressStr = '';
    while (inProgressOps.hasNext()) {
        let op = inProgressOps.next();
        inProgressStr += tojson(op);
        if (op.command.moveChunk) {
            admin.killOp(op.opid);
            abortedMigration = true;
        }
    }
    assert.eq(
        true, abortedMigration, "Failed to abort migration, current running ops: " + inProgressStr);
    unpauseMoveChunkAtStep(shard0, moveChunkStepNames.startedMoveChunk);
    assert.throws(function() {
        joinMoveChunk();
    });

    // Start coll2 migration to shard2, pause recipient after delete step.
    pauseMigrateAtStep(shard2, migrateStepNames.deletedPriorDataInRange);
    joinMoveChunk = moveChunkParallel(
        staticMongod, st.s0.host, {a: 0}, null, coll2.getFullName(), st.shard2.shardName);
    waitForMigrateStep(shard2, migrateStepNames.deletedPriorDataInRange);

    jsTest.log('Releasing coll1 migration recipient, whose clone command should fail....');
    unpauseMigrateAtStep(shard1, migrateStepNames.deletedPriorDataInRange);
    assert.soon(function() {
        // Wait for the destination shard to report that it is not in an active migration.
        var res = shard1.adminCommand({'_recvChunkStatus': 1});
        return (res.active == false);
    }, "coll1 migration recipient didn't abort migration in clone phase.", 2 * 60 * 1000);
    assert.eq(
        1, shard0Coll1.find().itcount(), "donor shard0 completed a migration that it aborted.");
    assert.eq(0,
              shard1Coll1.find().itcount(),
              "shard1 cloned documents despite donor migration abortion.");

    jsTest.log('Finishing coll2 migration, which should succeed....');
    unpauseMigrateAtStep(shard2, migrateStepNames.deletedPriorDataInRange);
    assert.doesNotThrow(function() {
        joinMoveChunk();
    });

    assert.eq(0,
              shard0Coll2.find().itcount(),
              "donor shard0 failed to complete a migration after aborting a prior migration.");
    assert.eq(1, shard2Coll2.find().itcount(), "shard2 failed to complete migration.");

    st.stop();
    MongoRunner.stopMongod(staticMongod);
})();
