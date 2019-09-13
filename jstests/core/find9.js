// @tags: [requires_getmore]

// Test that the MaxBytesToReturnToClientAtOnce limit is enforced.

t = db.jstests_find9;
t.drop();

big = new Array(500000).toString();
for (i = 0; i < 60; ++i) {
    t.save({a: i, b: big});
}

// Check size limit with a simple query.
assert.eq(60, t.find({}, {a: 1}).objsLeftInBatch());  // Projection resizes the result set.
assert.gt(60, t.find().objsLeftInBatch());

// Check size limit on a query with an explicit batch size.
assert.eq(60, t.find({}, {a: 1}).batchSize(80).objsLeftInBatch());
assert.gt(60, t.find().batchSize(80).objsLeftInBatch());

for (i = 0; i < 60; ++i) {
    t.save({a: i, b: big});
}

// Check size limit with get more.
c = t.find().batchSize(80);
while (c.hasNext()) {
    assert.gt(60, c.objsLeftInBatch());
    c.next();
}

assert.eq(120, t.find().sort({$natural: 1}).itcount());
assert.eq(120, t.find().sort({_id: 1}).itcount());
