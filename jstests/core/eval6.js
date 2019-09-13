// @tags: [
//   # Cannot implicitly shard accessed collections because unsupported use of sharded collection
//   # from db.eval.
//   assumes_unsharded_collection,
//   requires_eval_command,
//   requires_non_retryable_commands,
// ]

t = db.eval6;
t.drop();

t.save({a: 1});

db.eval(function() {
    o = db.eval6.findOne();
    o.b = 2;
    db.eval6.save(o);
});

assert.eq(2, t.findOne().b);
