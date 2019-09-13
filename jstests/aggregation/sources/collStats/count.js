// Test that count within a $collStats stage returns the correct number of documents.
// @tags: [assumes_no_implicit_collection_creation_after_drop]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For "assertErrorCode".
    load("jstests/libs/fixture_helpers.js");      // For "FixtureHelpers".

    let testDB = db.getSiblingDB("aggregation_count_db");
    let coll = testDB.aggregation_count;
    coll.drop();

    let nDocs = 1000;
    for (var i = 0; i < nDocs; i++) {
        assert.writeOK(coll.insert({a: i}));
    }

    // Test that $collStats must be first stage.
    let pipeline = [{$match: {}}, {$collStats: {}}];
    assertErrorCode(coll, pipeline, 40602);

    // Test that an error is returned if count is not an object.
    pipeline = [{$collStats: {count: 1}}];
    assertErrorCode(coll, pipeline, 40480, "count spec must be an object");
    pipeline = [{$collStats: {count: "1"}}];
    assertErrorCode(coll, pipeline, 40480, "count spec must be an object");

    // Test the accuracy of the record count as a standalone option.
    pipeline = [{$collStats: {count: {}}}];
    let result = coll.aggregate(pipeline).next();
    assert.eq(nDocs, result.count);

    // Test the record count alongside latencyStats and storageStats.
    pipeline = [{$collStats: {count: {}, latencyStats: {}}}];
    result = coll.aggregate(pipeline).next();
    assert.eq(nDocs, result.count);
    assert(result.hasOwnProperty("latencyStats"));
    assert(result.latencyStats.hasOwnProperty("reads"));
    assert(result.latencyStats.hasOwnProperty("writes"));
    assert(result.latencyStats.hasOwnProperty("commands"));

    pipeline = [{$collStats: {count: {}, latencyStats: {}, storageStats: {}}}];
    result = coll.aggregate(pipeline).next();
    assert.eq(nDocs, result.count);
    assert(result.hasOwnProperty("latencyStats"));
    assert(result.latencyStats.hasOwnProperty("reads"));
    assert(result.latencyStats.hasOwnProperty("writes"));
    assert(result.latencyStats.hasOwnProperty("commands"));
    assert(result.hasOwnProperty("storageStats"));
    assert.eq(nDocs, result.storageStats.count);

    // Test the record count against an empty collection.
    assert.writeOK(coll.remove({}));
    pipeline = [{$collStats: {count: {}}}];
    result = coll.aggregate(pipeline).next();
    assert.eq(0, result.count);

    // Test that we error when the collection does not exist.
    coll.drop();
    assertErrorCode(coll, pipeline, 40481);

    // Test that we error when the database does not exist.
    // TODO SERVER-33039 When running against a mongos, a non-existent database will cause all
    // aggregations to return an empty result set.
    assert.commandWorked(testDB.dropDatabase());
    if (FixtureHelpers.isMongos(testDB)) {
        assert.eq([], coll.aggregate(pipeline).toArray());
    } else {
        assertErrorCode(coll, pipeline, 40481);
    }
}());
