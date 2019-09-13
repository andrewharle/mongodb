// Tests running aggregations within a client session. This test was designed to reproduce
// SERVER-33660.
(function() {
    "use strict";

    const st = new ShardingTest({shards: 2});

    // Gate this test to transaction supporting engines only as it uses txnNumber.
    let shardDB = st.rs0.getPrimary().getDB("test");
    if (!shardDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        jsTestLog("Do not run on storage engine that does not support transactions");
        st.stop();
        return;
    }

    const session = st.s0.getDB("test").getMongo().startSession();
    const mongosColl = session.getDatabase("test")[jsTestName()];

    // Shard the collection, split it into two chunks, and move the [1, MaxKey] chunk to the other
    // shard. We need chunks distributed across multiple shards in order to force a split pipeline
    // merging on a mongod - otherwise the entire pipeline will be forwarded without a split and
    // without a $mergeCursors stage.
    st.shardColl(mongosColl, {_id: 1}, {_id: 1}, {_id: 1});
    assert.writeOK(mongosColl.insert([{_id: 0}, {_id: 1}, {_id: 2}]));

    // This assertion will reproduce the hang described in SERVER-33660.
    assert.eq(
        [{_id: 0}, {_id: 1}, {_id: 2}],
        mongosColl
            .aggregate([{$_internalSplitPipeline: {mergeType: "primaryShard"}}, {$sort: {_id: 1}}])
            .toArray());

    // Test a couple more aggregations to be sure.
    assert.eq(
        [{_id: 0}, {_id: 1}, {_id: 2}],
        mongosColl.aggregate([{$_internalSplitPipeline: {mergeType: "mongos"}}, {$sort: {_id: 1}}])
            .toArray());
    assert.eq(mongosColl.aggregate([{$sort: {_id: 1}}, {$out: "testing"}]).itcount(), 0);

    assert.commandWorked(mongosColl.getDB().runCommand(
        {aggregate: mongosColl.getName(), pipeline: [], cursor: {}, txnNumber: NumberLong(1)}));

    st.stop();
}());
