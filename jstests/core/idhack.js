// @tags: [requires_non_retryable_writes]
(function() {
    "use strict";

    const t = db.idhack;
    t.drop();

    // Include helpers for analyzing explain output.
    load("jstests/libs/analyze_plan.js");

    assert.writeOK(t.insert({_id: {x: 1}, z: 1}));
    assert.writeOK(t.insert({_id: {x: 2}, z: 2}));
    assert.writeOK(t.insert({_id: {x: 3}, z: 3}));
    assert.writeOK(t.insert({_id: 1, z: 4}));
    assert.writeOK(t.insert({_id: 2, z: 5}));
    assert.writeOK(t.insert({_id: 3, z: 6}));

    assert.eq(2, t.findOne({_id: {x: 2}}).z);
    assert.eq(2, t.find({_id: {$gte: 2}}).count());
    assert.eq(2, t.find({_id: {$gte: 2}}).itcount());

    t.update({_id: {x: 2}}, {$set: {z: 7}});
    assert.eq(7, t.findOne({_id: {x: 2}}).z);

    t.update({_id: {$gte: 2}}, {$set: {z: 8}}, false, true);
    assert.eq(4, t.findOne({_id: 1}).z);
    assert.eq(8, t.findOne({_id: 2}).z);
    assert.eq(8, t.findOne({_id: 3}).z);

    // explain output should show that the ID hack was applied.
    const query = {_id: {x: 2}};
    let explain = t.find(query).explain(true);
    assert.eq(1, explain.executionStats.nReturned);
    assert.eq(1, explain.executionStats.totalKeysExamined);
    assert(isIdhack(db, explain.queryPlanner.winningPlan));

    // ID hack cannot be used with hint().
    t.ensureIndex({_id: 1, a: 1});
    explain = t.find(query).hint({_id: 1, a: 1}).explain();
    assert(!isIdhack(db, explain.queryPlanner.winningPlan));

    // ID hack cannot be used with skip().
    explain = t.find(query).skip(1).explain();
    assert(!isIdhack(db, explain.queryPlanner.winningPlan));

    // ID hack cannot be used with a regex predicate.
    assert.writeOK(t.insert({_id: "abc"}));
    explain = t.find({_id: /abc/}).explain();
    assert.eq({_id: "abc"}, t.findOne({_id: /abc/}));
    assert(!isIdhack(db, explain.queryPlanner.winningPlan));

    // Covered query returning _id field only can be handled by ID hack.
    explain = t.find(query, {_id: 1}).explain();
    assert(isIdhack(db, explain.queryPlanner.winningPlan));
    // Check doc from covered ID hack query.
    assert.eq({_id: {x: 2}}, t.findOne(query, {_id: 1}));

    //
    // Non-covered projection for idhack.
    //

    t.drop();
    assert.writeOK(t.insert({_id: 0, a: 0, b: [{c: 1}, {c: 2}]}));
    assert.writeOK(t.insert({_id: 1, a: 1, b: [{c: 3}, {c: 4}]}));

    // Simple inclusion.
    assert.eq({_id: 1, a: 1}, t.find({_id: 1}, {a: 1}).next());
    assert.eq({a: 1}, t.find({_id: 1}, {_id: 0, a: 1}).next());
    assert.eq({_id: 0, a: 0}, t.find({_id: 0}, {_id: 1, a: 1}).next());

    // Non-simple: exclusion.
    assert.eq({_id: 1, a: 1}, t.find({_id: 1}, {b: 0}).next());
    assert.eq({_id: 0}, t.find({_id: 0}, {a: 0, b: 0}).next());

    // Non-simple: dotted fields.
    assert.eq({b: [{c: 1}, {c: 2}]}, t.find({_id: 0}, {_id: 0, "b.c": 1}).next());
    assert.eq({_id: 1}, t.find({_id: 1}, {"foo.bar": 1}).next());

    // Non-simple: elemMatch projection.
    assert.eq({_id: 1, b: [{c: 4}]}, t.find({_id: 1}, {b: {$elemMatch: {c: 4}}}).next());

    // Non-simple: .returnKey().
    assert.eq({_id: 1}, t.find({_id: 1}).returnKey().next());

    // Non-simple: .returnKey() overrides other projections.
    assert.eq({_id: 1}, t.find({_id: 1}, {a: 1}).returnKey().next());
})();
