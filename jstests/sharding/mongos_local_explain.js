/**
 * Test that a mongos-only aggregation pipeline is explainable, and that the resulting explain plan
 * confirms that the pipeline ran entirely on mongoS.
 */
(function() {
    "use strict";

    const st = new ShardingTest({name: "mongos_comment_test", mongos: 1, shards: 1});
    const mongosConn = st.s;

    // MongoS-only stages to be tested and the expected 'explain' output for that stage.
    const testStages = {
        "$listLocalSessions": {allUsers: false, users: [{user: "nobody", db: "nothing"}]},
        "$listLocalCursors": {}
    };

    for (let stage in testStages) {
        // Use the test stage to create a pipeline that runs exclusively on mongoS.
        const mongosOnlyPipeline = [{[stage]: testStages[stage]}, {$match: {dummyField: 1}}];

        // We expect the explain output to reflect the stage's spec.
        const expectedExplainStages =
            [{[stage]: testStages[stage]}, {$match: {dummyField: {$eq: 1}}}];

        // Test that the mongoS-only pipeline is explainable.
        const explainPlan = assert.commandWorked(mongosConn.getDB("admin").runCommand(
            {aggregate: 1, pipeline: mongosOnlyPipeline, explain: true}));

        // We expect the stages to appear under the 'mongos' heading, for 'splitPipeline' to be
        // null, and for the 'mongos.host' field to be the hostname:port of the mongoS itself.
        assert.docEq(explainPlan.mongos.stages, expectedExplainStages);
        assert.eq(explainPlan.mongos.host, mongosConn.name);
        assert.isnull(explainPlan.splitPipeline);
    }

    st.stop();
})();