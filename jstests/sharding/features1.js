(function() {
    'use strict';

    var s = new ShardingTest({name: "features1", shards: 2, mongos: 1});
    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', s.shard1.shardName);

    // ---- can't shard system namespaces ----
    assert.commandFailed(s.s0.adminCommand({shardcollection: "test.system.blah", key: {num: 1}}),
                         "shard system namespace");

    // ---- setup test.foo -----
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {num: 1}}));
    let db = s.s0.getDB("test");

    assert.commandWorked(db.foo.createIndex({y: 1}));

    assert.commandWorked(s.s0.adminCommand({split: "test.foo", middle: {num: 10}}));
    assert.commandWorked(s.s0.adminCommand(
        {movechunk: "test.foo", find: {num: 20}, to: s.getOther(s.getPrimaryShard("test")).name}));

    assert.writeOK(db.foo.insert({num: 5}));
    assert.writeOK(db.foo.save({num: 15}));

    let a = s.rs0.getPrimary().getDB("test");
    let b = s.rs1.getPrimary().getDB("test");

    // ---- make sure shard key index is everywhere ----
    assert.eq(3, a.foo.getIndexKeys().length, "a index 1");
    assert.eq(3, b.foo.getIndexKeys().length, "b index 1");

    // ---- make sure if you add an index it goes everywhere ------
    assert.commandWorked(db.foo.createIndex({x: 1}));
    assert.eq(4, a.foo.getIndexKeys().length, "a index 2");
    assert.eq(4, b.foo.getIndexKeys().length, "b index 2");

    // ---- no unique indexes allowed that do not include the shard key ------
    assert.commandFailed(db.foo.createIndex({z: 1}, true));
    assert.eq(4, a.foo.getIndexKeys().length, "a index 3");
    assert.eq(4, b.foo.getIndexKeys().length, "b index 3");

    // ---- unique indexes that include the shard key are allowed ------
    assert.commandWorked(db.foo.createIndex({num: 1, bar: 1}, true));
    assert.eq(5, b.foo.getIndexKeys().length, "c index 3");

    // ---- can't shard thing with unique indexes ------
    assert.commandWorked(db.foo2.createIndex({a: 1}));
    printjson(db.foo2.getIndexes());
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo2", key: {num: 1}}),
                         "shard with index");

    assert.commandWorked(db.foo3.createIndex({a: 1}, true));
    printjson(db.foo3.getIndexes());
    assert.commandFailed(s.s0.adminCommand({shardcollection: "test.foo3", key: {num: 1}}),
                         "shard with unique index");

    assert.commandWorked(db.foo7.createIndex({num: 1, a: 1}, true));
    printjson(db.foo7.getIndexes());
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo7", key: {num: 1}}),
                         "shard with ok unique index");

    // ----- eval -----

    db.foo2.save({num: 5, a: 7});
    db.foo3.save({num: 5, a: 8});

    assert.eq(1, db.foo3.count(), "eval pre1");
    assert.eq(1, db.foo2.count(), "eval pre2");

    assert.eq(8,
              db.eval(function() {
                  return db.foo3.findOne().a;
              }),
              "eval 1 ");
    assert.throws(function() {
        db.eval(function() {
            return db.foo2.findOne().a;
        });
    }, [], "eval 2");

    assert.eq(1,
              db.eval(function() {
                  return db.foo3.count();
              }),
              "eval 3 ");
    assert.throws(function() {
        db.eval(function() {
            return db.foo2.count();
        });
    }, [], "eval 4");

    // ----- "eval" new command name SERVER-5588 -----
    var result;
    result = db.runCommand({
        eval: function() {
            return db.foo3.count();
        }
    });  // non-sharded collection
    assert.eq(1, result.ok, "eval should work for non-sharded collection in cluster");

    result = db.runCommand({
        eval: function() {
            return db.foo2.count();
        }
    });  // sharded collection
    assert.eq(0, result.ok, "eval should not work for sharded collection in cluster");

    // ---- unique shard key ----
    assert.commandWorked(
        s.s0.adminCommand({shardcollection: "test.foo4", key: {num: 1}, unique: true}),
        "shard with index and unique");
    assert.commandWorked(s.s0.adminCommand({split: "test.foo4", middle: {num: 10}}));
    assert.commandWorked(s.s0.adminCommand(
        {movechunk: "test.foo4", find: {num: 20}, to: s.getOther(s.getPrimaryShard("test")).name}));

    assert.writeOK(db.foo4.insert({num: 5}));
    assert.writeOK(db.foo4.insert({num: 15}));

    assert.eq(1, a.foo4.count(), "ua1");
    assert.eq(1, b.foo4.count(), "ub1");

    assert.eq(2, a.foo4.getIndexes().length, "ua2");
    assert.eq(2, b.foo4.getIndexes().length, "ub2");

    assert(a.foo4.getIndexes()[1].unique, "ua3");
    assert(b.foo4.getIndexes()[1].unique, "ub3");

    assert.eq(2, db.foo4.count(), "uc1");
    assert.writeOK(db.foo4.insert({num: 7}));
    assert.eq(3, db.foo4.count(), "uc2");
    assert.writeError(db.foo4.insert({num: 7}));
    assert.eq(3, db.foo4.count(), "uc4");

    // --- don't let you convertToCapped ----
    assert(!db.foo4.isCapped(), "ca1");
    assert(!a.foo4.isCapped(), "ca2");
    assert(!b.foo4.isCapped(), "ca3");

    assert.commandFailed(db.foo4.convertToCapped(30000), "ca30");
    assert(!db.foo4.isCapped(), "ca4");
    assert(!a.foo4.isCapped(), "ca5");
    assert(!b.foo4.isCapped(), "ca6");

    //      make sure i didn't break anything
    db.foo4a.save({a: 1});
    assert(!db.foo4a.isCapped(), "ca7");
    db.foo4a.convertToCapped(30000);
    assert(db.foo4a.isCapped(), "ca8");

    // --- don't let you shard a capped collection
    db.createCollection("foo5", {capped: true, size: 30000});
    assert(db.foo5.isCapped(), "cb1");
    assert.commandFailed(s.s0.adminCommand({shardcollection: "test.foo5", key: {num: 1}}));

    // ----- group ----

    db.foo6.save({a: 1});
    db.foo6.save({a: 3});
    db.foo6.save({a: 3});
    db.foo6.ensureIndex({a: 1});
    s.sync();
    printjson(db.foo6.getIndexes());

    assert.eq(2,
              db.foo6
                  .group({
                      key: {a: 1},
                      initial: {count: 0},
                      reduce: function(z, prev) {
                          prev.count++;
                      }
                  })
                  .length);

    assert.eq(3, db.foo6.find().count());
    assert(s.admin.runCommand({shardcollection: "test.foo6", key: {a: 1}}).ok);
    assert.eq(3, db.foo6.find().count());

    s.adminCommand({split: "test.foo6", middle: {a: 2}});

    // movechunk commands are wrapped in assert.soon
    // Sometimes the TO-side shard isn't immediately ready, this
    // causes problems on slow hosts.
    // Remove when SERVER-10232 is fixed

    assert.soon(function() {
        var cmdRes = s.admin.runCommand(
            {movechunk: "test.foo6", find: {a: 3}, to: s.getOther(s.getPrimaryShard("test")).name});
        return cmdRes.ok;
    }, 'move chunk test.foo6', 60000, 1000);

    assert.throws(function() {
        db.foo6.group({
            key: {a: 1},
            initial: {count: 0},
            reduce: function(z, prev) {
                prev.count++;
            }
        });
    });

    // ---- can't shard non-empty collection without index -----
    assert.writeOK(db.foo8.insert({a: 1}));
    assert.commandFailed(s.s0.adminCommand({shardcollection: "test.foo8", key: {a: 1}}),
                         "non-empty collection");

    // ---- can't shard non-empty collection with null values in shard key ----
    assert.writeOK(db.foo9.insert({b: 1}));
    assert.commandWorked(db.foo9.createIndex({a: 1}));
    assert.commandFailed(s.s0.adminCommand({shardcollection: "test.foo9", key: {a: 1}}),
                         "entry with null value");

    // --- listDatabases ---
    var r = db.getMongo().getDBs();
    assert.eq(3, r.databases.length, tojson(r));
    assert.eq("number", typeof(r.totalSize), "listDatabases 3 : " + tojson(r));

    // --- flushRouterconfig ---
    assert.commandWorked(s.s0.adminCommand({flushRouterConfig: 1}));
    assert.commandWorked(s.s0.adminCommand({flushRouterConfig: true}));
    assert.commandWorked(s.s0.adminCommand({flushRouterConfig: 'TestDB'}));
    assert.commandWorked(s.s0.adminCommand({flushRouterConfig: 'TestDB.TestColl'}));

    s.stop();
})();
