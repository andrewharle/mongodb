(function() {
    "use strict";

    load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.
    load("jstests/libs/analyze_plan.js");     // For planHasStage.

    var coll = db.distinct_multikey_index;

    coll.drop();
    for (var i = 0; i < 10; i++) {
        assert.writeOK(coll.save({a: 1, b: 1}));
        assert.writeOK(coll.save({a: 1, b: 2}));
        assert.writeOK(coll.save({a: 2, b: 1}));
        assert.writeOK(coll.save({a: 2, b: 3}));
    }
    coll.createIndex({a: 1, b: 1});

    var explain_distinct_with_query = coll.explain("executionStats").distinct('b', {a: 1});
    assert.commandWorked(explain_distinct_with_query);
    assert(planHasStage(db, explain_distinct_with_query.queryPlanner.winningPlan, "DISTINCT_SCAN"));
    assert(planHasStage(db, explain_distinct_with_query.queryPlanner.winningPlan, "PROJECTION"));
    // If the collection is sharded, we expect at most 2 distinct values per shard. If the
    // collection is not sharded, we expect 2 returned.
    assert.lte(explain_distinct_with_query.executionStats.nReturned,
               2 * FixtureHelpers.numberOfShardsForCollection(coll));

    var explain_distinct_without_query = coll.explain("executionStats").distinct('b');
    assert.commandWorked(explain_distinct_without_query);
    assert(planHasStage(db, explain_distinct_without_query.queryPlanner.winningPlan, "COLLSCAN"));
    assert(!planHasStage(
        db, explain_distinct_without_query.queryPlanner.winningPlan, "DISTINCT_SCAN"));
    assert.eq(40, explain_distinct_without_query.executionStats.nReturned);
})();
