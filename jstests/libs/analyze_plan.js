// Contains helpers for checking, based on the explain output, properties of a
// plan. For instance, there are helpers for checking whether a plan is a collection
// scan or whether the plan is covered (index only).

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

/**
 * Given the root stage of explain's JSON representation of a query plan ('root'), returns all
 * subdocuments whose stage is 'stage'. Returns an empty array if the plan does not have the
 * requested stage.
 */
function getPlanStages(root, stage) {
    var results = [];

    if (root.stage === stage) {
        results.push(root);
    }

    if ("inputStage" in root) {
        results = results.concat(getPlanStages(root.inputStage, stage));
    }

    if ("inputStages" in root) {
        for (var i = 0; i < root.inputStages.length; i++) {
            results = results.concat(getPlanStages(root.inputStages[i], stage));
        }
    }

    if ("shards" in root) {
        for (var i = 0; i < root.shards.length; i++) {
            if ("winningPlan" in root.shards[i]) {
                results = results.concat(getPlanStages(root.shards[i].winningPlan, stage));
            } else {
                results = results.concat(getPlanStages(root.shards[i].executionStages, stage));
            }
        }
    }

    return results;
}

/**
 * Given the root stage of explain's JSON representation of a query plan ('root'), returns the
 * subdocument with its stage as 'stage'. Returns null if the plan does not have such a stage.
 * Asserts that no more than one stage is a match.
 */
function getPlanStage(root, stage) {
    var planStageList = getPlanStages(root, stage);

    if (planStageList.length === 0) {
        return null;
    } else {
        assert(planStageList.length === 1,
               "getPlanStage expects to find 0 or 1 matching stages. planStageList: " +
                   tojson(planStageList));
        return planStageList[0];
    }
}

/**
 * Given the root stage of explain's JSON representation of a query plan ('root'), returns true if
 * the query planner reports at least one rejected alternative plan, and false otherwise.
 */
function hasRejectedPlans(root) {
    function sectionHasRejectedPlans(explainSection) {
        assert(explainSection.hasOwnProperty("rejectedPlans"), tojson(explainSection));
        return explainSection.rejectedPlans.length !== 0;
    }

    function cursorStageHasRejectedPlans(cursorStage) {
        assert(cursorStage.hasOwnProperty("$cursor"), tojson(cursorStage));
        assert(cursorStage.$cursor.hasOwnProperty("queryPlanner"), tojson(cursorStage));
        return sectionHasRejectedPlans(cursorStage.$cursor.queryPlanner);
    }

    if (root.hasOwnProperty("shards")) {
        // This is a sharded agg explain.
        const cursorStages = getAggPlanStages(root, "$cursor");
        assert(cursorStages.length !== 0, "Did not find any $cursor stages in sharded agg explain");
        return cursorStages.find((cursorStage) => cursorStageHasRejectedPlans(cursorStage)) !==
            undefined;
    } else if (root.hasOwnProperty("stages")) {
        // This is an agg explain.
        const cursorStages = getAggPlanStages(root, "$cursor");
        return cursorStages.find((cursorStage) => cursorStageHasRejectedPlans(cursorStage)) !==
            undefined;
    } else {
        // This is some sort of query explain.
        assert(root.hasOwnProperty("queryPlanner"), tojson(root));
        assert(root.queryPlanner.hasOwnProperty("winningPlan"), tojson(root));
        if (!root.queryPlanner.winningPlan.hasOwnProperty("shards")) {
            // This is an unsharded explain.
            return sectionHasRejectedPlans(root.queryPlanner);
        }
        // This is a sharded explain. Each entry in the shards array contains a 'winningPlan' and
        // 'rejectedPlans'.
        return root.queryPlanner.winningPlan.shards.find(
                   (shard) => sectionHasRejectedPlans(shard)) !== undefined;
    }
}

/**
 * Given the root stage of agg explain's JSON representation of a query plan ('root'), returns all
 * subdocuments whose stage is 'stage'. This can either be an agg stage name like "$cursor" or
 * "$sort", or a query stage name like "IXSCAN" or "SORT".
 *
 * Returns an empty array if the plan does not have the requested stage. Asserts that agg explain
 * structure matches expected format.
 */
function getAggPlanStages(root, stage) {
    let results = [];

    function getDocumentSources(docSourceArray) {
        let results = [];
        for (let i = 0; i < docSourceArray.length; i++) {
            let properties = Object.getOwnPropertyNames(docSourceArray[i]);
            assert.eq(1, properties.length);
            if (properties[0] === stage) {
                results.push(docSourceArray[i]);
            }
        }
        return results;
    }

    if (root.hasOwnProperty("stages")) {
        assert(root.stages.constructor === Array);

        results = results.concat(getDocumentSources(root.stages));

        assert(root.stages[0].hasOwnProperty("$cursor"));
        assert(root.stages[0].$cursor.hasOwnProperty("queryPlanner"));
        assert(root.stages[0].$cursor.queryPlanner.hasOwnProperty("winningPlan"));
        results =
            results.concat(getPlanStages(root.stages[0].$cursor.queryPlanner.winningPlan, stage));
    }

    if (root.hasOwnProperty("shards")) {
        for (let elem in root.shards) {
            assert(root.shards[elem].stages.constructor === Array);

            results = results.concat(getDocumentSources(root.shards[elem].stages));

            assert(root.shards[elem].stages[0].hasOwnProperty("$cursor"));
            assert(root.shards[elem].stages[0].$cursor.hasOwnProperty("queryPlanner"));
            assert(root.shards[elem].stages[0].$cursor.queryPlanner.hasOwnProperty("winningPlan"));
            results = results.concat(
                getPlanStages(root.shards[elem].stages[0].$cursor.queryPlanner.winningPlan, stage));
        }
    }

    return results;
}

/**
 * Given the root stage of agg explain's JSON representation of a query plan ('root'), returns the
 * subdocument with its stage as 'stage'. Returns null if the plan does not have such a stage.
 * Asserts that no more than one stage is a match.
 */
function getAggPlanStage(root, stage) {
    let planStageList = getAggPlanStages(root, stage);

    if (planStageList.length === 0) {
        return null;
    } else {
        assert.eq(1,
                  planStageList.length,
                  "getAggPlanStage expects to find 0 or 1 matching stages. planStageList: " +
                      tojson(planStageList));
        return planStageList[0];
    }
}

/**
 * Given the root stage of agg explain's JSON representation of a query plan ('root'), returns
 * whether the plan as stage called 'stage'.
 */
function aggPlanHasStage(root, stage) {
    return getAggPlanStage(root, stage) !== null;
}

/**
 * Given the root stage of explain's BSON representation of a query plan ('root'),
 * returns true if the plan has a stage called 'stage'.
 */
function planHasStage(db, root, stage) {
    const matchingStages = getPlanStages(root, stage);

    // If we are executing against a mongos, we may get more than one occurrence of the stage.
    if (FixtureHelpers.isMongos(db)) {
        return matchingStages.length >= 1;
    } else {
        assert.lt(matchingStages.length,
                  2,
                  `Expected to find 0 or 1 matching stages: ${tojson(matchingStages)}`);
        return matchingStages.length === 1;
    }
}

/**
 * A query is covered iff it does *not* have a FETCH stage or a COLLSCAN.
 *
 * Given the root stage of explain's BSON representation of a query plan ('root'),
 * returns true if the plan is index only. Otherwise returns false.
 */
function isIndexOnly(db, root) {
    return !planHasStage(db, root, "FETCH") && !planHasStage(db, root, "COLLSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * an index scan, and false otherwise.
 */
function isIxscan(db, root) {
    return planHasStage(db, root, "IXSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * the idhack fast path, and false otherwise.
 */
function isIdhack(db, root) {
    return planHasStage(db, root, "IDHACK");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * a collection scan, and false otherwise.
 */
function isCollscan(db, root) {
    return planHasStage(db, root, "COLLSCAN");
}

/**
 * Get the number of chunk skips for the BSON exec stats tree rooted at 'root'.
 */
function getChunkSkips(root) {
    if (root.stage === "SHARDING_FILTER") {
        return root.chunkSkips;
    } else if ("inputStage" in root) {
        return getChunkSkips(root.inputStage);
    } else if ("inputStages" in root) {
        var skips = 0;
        for (var i = 0; i < root.inputStages.length; i++) {
            skips += getChunkSkips(root.inputStages[0]);
        }
        return skips;
    }

    return 0;
}

/**
 * Given explain output at executionStats level verbosity, confirms that the root stage is COUNT and
 * that the result of the count is equal to 'expectedCount'.
 */
function assertExplainCount({explainResults, expectedCount}) {
    const execStages = explainResults.executionStats.executionStages;

    // If passed through mongos, then the root stage should be the mongos SINGLE_SHARD stage or
    // SHARD_MERGE stages, with COUNT as the root stage on each shard. If explaining directly on the
    // shard, then COUNT is the root stage.
    if ("SINGLE_SHARD" == execStages.stage || "SHARD_MERGE" == execStages.stage) {
        let totalCounted = 0;
        for (let shardExplain of execStages.shards) {
            const countStage = shardExplain.executionStages;
            assert.eq(countStage.stage, "COUNT", "root stage on shard is not COUNT");
            totalCounted += countStage.nCounted;
        }
        assert.eq(totalCounted, expectedCount, "wrong count result");
    } else {
        assert.eq(execStages.stage, "COUNT", "root stage is not COUNT");
        assert.eq(execStages.nCounted, expectedCount, "wrong count result");
    }
}

/**
 * Verifies that a given query uses an index and is covered when used in a count command.
 */
function assertCoveredQueryAndCount({collection, query, project, count}) {
    let explain = collection.find(query, project).explain();
    assert(isIndexOnly(db, explain.queryPlanner.winningPlan),
           "Winning plan was not covered: " + tojson(explain.queryPlanner.winningPlan));

    // Same query as a count command should also be covered.
    explain = collection.explain("executionStats").find(query).count();
    assert(isIndexOnly(db, explain.queryPlanner.winningPlan),
           "Winning plan for count was not covered: " + tojson(explain.queryPlanner.winningPlan));
    assertExplainCount({explainResults: explain, expectedCount: count});
}
