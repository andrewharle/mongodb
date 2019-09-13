// @tags: [
//   # Cannot implicitly shard accessed collections because of following errmsg: A single
//   # update/delete on a sharded collection must contain an exact match on _id or contain the shard
//   # key.
//   assumes_unsharded_collection,
//   # This test attempts to perform write operations and get index usage statistics using the
//   # $indexStats stage. The former operation must be routed to the primary in a replica set,
//   # whereas the latter may be routed to a secondary.
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
//   requires_non_retryable_writes,
// ]

(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    var colName = "jstests_index_stats";
    var col = db[colName];
    col.drop();

    var getUsageCount = function(indexName, collection) {
        collection = collection || col;
        var cursor = collection.aggregate([{$indexStats: {}}]);
        while (cursor.hasNext()) {
            var doc = cursor.next();

            if (doc.name === indexName) {
                return doc.accesses.ops;
            }
        }

        return undefined;
    };

    var getIndexKey = function(indexName) {
        var cursor = col.aggregate([{$indexStats: {}}]);
        while (cursor.hasNext()) {
            var doc = cursor.next();

            if (doc.name === indexName) {
                return doc.key;
            }
        }

        return undefined;
    };

    var getIndexNamesForWinningPlan = function(explain) {
        var indexNameList = [];
        var winningStages = getPlanStages(explain.queryPlanner.winningPlan, "IXSCAN");
        for (var i = 0; i < winningStages.length; ++i) {
            indexNameList.push(winningStages[i].indexName);
        }

        return indexNameList;
    };

    assert.writeOK(col.insert({a: 1, b: 1, c: 1}));
    assert.writeOK(col.insert({a: 2, b: 2, c: 2}));
    assert.writeOK(col.insert({a: 3, b: 3, c: 3}));

    //
    // Confirm no index stats object exists prior to index creation.
    //
    col.findOne({a: 1});
    assert.eq(undefined, getUsageCount("a_1"));

    //
    // Create indexes.
    //
    assert.commandWorked(col.createIndex({a: 1}, {name: "a_1"}));
    assert.commandWorked(col.createIndex({b: 1, c: 1}, {name: "b_1_c_1"}));
    var countA = 0;  // Tracks expected index access for "a_1".
    var countB = 0;  // Tracks expected index access for "b_1_c_1".

    //
    // Confirm a stats object exists post index creation (with 0 count).
    //
    assert.eq(countA, getUsageCount("a_1"));
    assert.eq({a: 1}, getIndexKey("a_1"));

    //
    // Confirm index stats tick on find().
    //
    col.findOne({a: 1});
    countA++;

    assert.eq(countA, getUsageCount("a_1"));

    //
    // Confirm index stats tick on findAndModify() update.
    //
    var res =
        db.runCommand({findAndModify: colName, query: {a: 1}, update: {$set: {d: 1}}, 'new': true});
    assert.commandWorked(res);
    countA++;
    assert.eq(countA, getUsageCount("a_1"));

    //
    // Confirm index stats tick on findAndModify() delete.
    //
    res = db.runCommand({findAndModify: colName, query: {a: 2}, remove: true});
    assert.commandWorked(res);
    countA++;
    assert.eq(countA, getUsageCount("a_1"));
    assert.writeOK(col.insert(res.value));

    //
    // Confirm $and operation ticks indexes for winning plan, but not rejected plans.
    //

    // Run explain to determine which indexes would be used for this query. Note that index
    // access counters are not incremented for explain execution.
    var explain = col.find({a: 2, b: 2}).explain("queryPlanner");
    var indexNameList = getIndexNamesForWinningPlan(explain);
    assert.gte(indexNameList.length, 1);

    for (var i = 0; i < indexNameList.length; ++i) {
        // Increment the expected $indexStats count for each index used.
        var name = indexNameList[i];
        if (name === "a_1") {
            countA++;
        } else {
            assert(name === "b_1_c_1");
            countB++;
        }
    }

    // Run the query again without explain to increment index access counters.
    col.findOne({a: 2, b: 2});
    // Check all indexes for proper count.
    assert.eq(countA, getUsageCount("a_1"));
    assert.eq(countB, getUsageCount("b_1_c_1"));
    assert.eq(0, getUsageCount("_id_"));

    //
    // Confirm index stats tick on distinct().
    //
    res = db.runCommand({distinct: colName, key: "b", query: {b: 1}});
    assert.commandWorked(res);
    countB++;
    assert.eq(countB, getUsageCount("b_1_c_1"));

    //
    // Confirm index stats tick on group().
    //
    res = db.runCommand({
        group: {
            ns: colName,
            key: {b: 1, c: 1},
            cond: {b: {$gt: 0}},
            $reduce: function(curr, result) {},
            initial: {}
        }
    });
    assert.commandWorked(res);
    countB++;
    assert.eq(countB, getUsageCount("b_1_c_1"));

    //
    // Confirm index stats tick on aggregate w/ match.
    //
    res = db.runCommand({aggregate: colName, pipeline: [{$match: {b: 1}}], cursor: {}});
    assert.commandWorked(res);
    countB++;
    assert.eq(countB, getUsageCount("b_1_c_1"));

    //
    // Confirm index stats tick on mapReduce with query.
    //
    res = db.runCommand({
        mapReduce: colName,
        map: function() {
            emit(this.b, this.c);
        },
        reduce: function(key, val) {
            return val;
        },
        query: {b: 2},
        out: {inline: true}
    });
    assert.commandWorked(res);
    countB++;
    assert.eq(countB, getUsageCount("b_1_c_1"));

    //
    // Confirm index stats tick on update().
    //
    assert.writeOK(col.update({a: 2}, {$set: {d: 2}}));
    countA++;
    assert.eq(countA, getUsageCount("a_1"));

    //
    // Confirm index stats tick on remove().
    //
    assert.writeOK(col.remove({a: 2}));
    countA++;
    assert.eq(countA, getUsageCount("a_1"));

    //
    // Confirm multiple index $or operation ticks all involved indexes.
    //
    col.findOne({$or: [{a: 1}, {b: 1, c: 1}]});
    countA++;
    countB++;
    assert.eq(countA, getUsageCount("a_1"));
    assert.eq(countB, getUsageCount("b_1_c_1"));

    //
    // Confirm index stats object does not exist post index drop.
    //
    assert.commandWorked(col.dropIndex("b_1_c_1"));
    countB = 0;
    assert.eq(undefined, getUsageCount("b_1_c_1"));

    //
    // Confirm index stats object exists with count 0 once index is recreated.
    //
    assert.commandWorked(col.createIndex({b: 1, c: 1}, {name: "b_1_c_1"}));
    assert.eq(countB, getUsageCount("b_1_c_1"));

    //
    // Confirm that retrieval fails if $indexStats is not in the first pipeline position.
    //
    assert.throws(function() {
        col.aggregate([{$match: {}}, {$indexStats: {}}]);
    });

    //
    // Confirm index use is recorded for $lookup.
    //
    const foreignCollection = db[colName + "_foreign"];
    foreignCollection.drop();
    assert.writeOK(foreignCollection.insert([{_id: 0}, {_id: 1}, {_id: 2}]));
    col.drop();
    assert.writeOK(col.insert([{_id: 0, foreignId: 1}, {_id: 1, foreignId: 2}]));
    assert.eq(0, getUsageCount("_id_"));
    assert.eq(2,
              col.aggregate([
                     {$match: {_id: {$in: [0, 1]}}},
                     {
                       $lookup: {
                           from: foreignCollection.getName(),
                           localField: 'foreignId',
                           foreignField: '_id',
                           as: 'results'
                       }
                     }
                 ])
                  .itcount());
    assert.eq(1, getUsageCount("_id_", col), "Expected aggregation to use _id index");
    assert.eq(2,
              getUsageCount("_id_", foreignCollection),
              "Expected each lookup to be tracked as an index use");

    //
    // Confirm index use is recorded for $graphLookup.
    //
    foreignCollection.drop();
    assert.writeOK(foreignCollection.insert([
        {_id: 0, connectedTo: 1},
        {_id: 1, connectedTo: "X"},
        {_id: 2, connectedTo: 3},
        {_id: 3, connectedTo: "Y"},  // Be sure to use a different value here to make sure
                                     // $graphLookup doesn't cache the query.
    ]));
    col.drop();
    assert.writeOK(col.insert([{_id: 0, foreignId: 0}, {_id: 1, foreignId: 2}]));
    assert.eq(0, getUsageCount("_id_"));
    assert.eq(2,
              col.aggregate([
                     {$match: {_id: {$in: [0, 1]}}},
                     {
                       $graphLookup: {
                           from: foreignCollection.getName(),
                           startWith: '$foreignId',
                           connectToField: '_id',
                           connectFromField: 'connectedTo',
                           as: 'results'
                       }
                     }
                 ])
                  .itcount());
    assert.eq(1, getUsageCount("_id_", col), "Expected aggregation to use _id index");
    assert.eq(2 * 3,
              getUsageCount("_id_", foreignCollection),
              "Expected each of two graph searches to issue 3 queries, each using the _id index");
})();
