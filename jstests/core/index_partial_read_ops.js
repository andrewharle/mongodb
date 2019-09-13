// Cannot implicitly shard accessed collections because the explain output from a mongod when run
// against a sharded collection is wrapped in a "shards" object with keys for each shard.
// @tags: [assumes_unsharded_collection, does_not_support_stepdowns]

// Read ops tests for partial indexes.

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

(function() {
    "use strict";
    var explain;
    var coll = db.index_partial_read_ops;
    coll.drop();

    assert.commandWorked(coll.ensureIndex({x: 1}, {partialFilterExpression: {a: {$lte: 1.5}}}));
    assert.writeOK(coll.insert({x: 5, a: 2}));  // Not in index.
    assert.writeOK(coll.insert({x: 6, a: 1}));  // In index.

    //
    // Verify basic functionality with find().
    //

    // find() operations that should use index.
    explain = coll.explain('executionStats').find({x: 6, a: 1}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, explain.queryPlanner.winningPlan));
    explain = coll.explain('executionStats').find({x: {$gt: 1}, a: 1}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, explain.queryPlanner.winningPlan));
    explain = coll.explain('executionStats').find({x: 6, a: {$lte: 1}}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, explain.queryPlanner.winningPlan));

    // find() operations that should not use index.
    explain = coll.explain('executionStats').find({x: 6, a: {$lt: 1.6}}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, explain.queryPlanner.winningPlan));
    explain = coll.explain('executionStats').find({x: 6}).finish();
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, explain.queryPlanner.winningPlan));

    //
    // Verify basic functionality with the count command.
    //

    // Count operation that should use index.
    explain = coll.explain('executionStats').count({x: {$gt: 1}, a: 1});
    assert(isIxscan(db, explain.queryPlanner.winningPlan));

    // Count operation that should not use index.
    explain = coll.explain('executionStats').count({x: {$gt: 1}, a: 2});
    assert(isCollscan(db, explain.queryPlanner.winningPlan));

    //
    // Verify basic functionality with the aggregate command.
    //

    // Aggregate operation that should use index.
    explain = coll.aggregate([{$match: {x: {$gt: 1}, a: 1}}], {explain: true}).stages[0].$cursor;
    assert(isIxscan(db, explain.queryPlanner.winningPlan));

    // Aggregate operation that should not use index.
    explain = coll.aggregate([{$match: {x: {$gt: 1}, a: 2}}], {explain: true}).stages[0].$cursor;
    assert(isCollscan(db, explain.queryPlanner.winningPlan));

    //
    // Verify basic functionality with the findAndModify command.
    //

    // findAndModify operation that should use index.
    explain = coll.explain('executionStats')
                  .findAndModify({query: {x: {$gt: 1}, a: 1}, update: {$inc: {x: 1}}});
    assert.eq(1, explain.executionStats.nReturned);
    assert(isIxscan(db, explain.queryPlanner.winningPlan));

    // findAndModify operation that should not use index.
    explain = coll.explain('executionStats')
                  .findAndModify({query: {x: {$gt: 1}, a: 2}, update: {$inc: {x: 1}}});
    assert.eq(1, explain.executionStats.nReturned);
    assert(isCollscan(db, explain.queryPlanner.winningPlan));
})();
