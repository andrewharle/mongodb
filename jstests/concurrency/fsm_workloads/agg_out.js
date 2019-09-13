'use strict';

/**
 * agg_out.js
 *
 * This test runs many concurrent aggregations using $out, writing to the same collection. While
 * this is happening, other threads may be creating or dropping indexes, changing the collection
 * options, or sharding the collection. We expect an aggregate with a $out stage to fail if another
 * client executed one of these changes between the creation of $out's temporary collection and the
 * eventual rename to the target collection.
 *
 * Unfortunately, there aren't very many assertions we can make here, so this is mostly to test that
 * the server doesn't deadlock or crash.
 *
 * @tags: [requires_capped]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');           // for extendWorkload
load('jstests/concurrency/fsm_workloads/agg_base.js');             // for $config
load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isMongos

var $config = extendWorkload($config, function($config, $super) {

    // Use a smaller document size, but more iterations. The smaller documents will ensure each
    // operation is faster, giving us time to do more operations and thus increasing the likelihood
    // that any two operations will be happening concurrently.
    $config.data.docSize = 1000;
    $config.iterations = 100;

    $config.data.outputCollName = 'agg_out';  // Use the workload name as the collection name
                                              // because it is assumed to be unique.

    $config.data.indexSpecs = [{rand: -1, randInt: 1}, {randInt: -1}, {flag: 1}, {padding: 'text'}];

    // We'll use document validation so that we can change the collection options in the middle of
    // an $out, to test that the $out stage will notice this and error. This validator is not very
    // interesting, and documents should always pass.
    $config.data.documentValidator = {flag: {$exists: true}};

    $config.transitions = {
        query: {
            query: 0.68,
            ensureIndexes: 0.1,
            dropIndex: 0.1,
            collMod: 0.1,
            // Converting the target collection to a capped collection or a sharded collection will
            // cause all subsequent aggregations to fail, so give these a low probability to make
            // sure they don't happen too early in the test.
            convertToCapped: 0.01,
            shardCollection: 0.01,
        },
        ensureIndexes: {query: 1},
        dropIndex: {query: 1},
        collMod: {query: 1},
        convertToCapped: {query: 1},
        shardCollection: {query: 1},
    };

    /**
     * Runs an aggregate with a $out into '$config.data.outputCollName'.
     */
    $config.states.query = function query(db, collName) {
        const res = db[collName].runCommand({
            aggregate: collName,
            pipeline: [{$match: {flag: true}}, {$out: this.outputCollName}],
            cursor: {}
        });

        if (res.ok) {
            const cursor = new DBCommandCursor(db, res);
            assertAlways.eq(0, cursor.itcount());  // No matter how many documents were in the
                                                   // original input stream, $out should never
                                                   // return any results.
        }
    };

    /**
     * Ensures all the indexes exist. This will have no affect unless some thread has already
     * dropped an index.
     */
    $config.states.ensureIndexes = function ensureIndexes(db, unusedCollName) {
        for (var i = 0; i < this.indexSpecs; ++i) {
            assertWhenOwnDB.commandWorked(db[this.outputCollName].ensureIndex(this.indexSpecs[i]));
        }
    };

    /**
     * Drops a random index from '$config.data.indexSpecs'.
     */
    $config.states.dropIndex = function dropIndex(db, unusedCollName) {
        const indexSpec = this.indexSpecs[Random.randInt(this.indexSpecs.length)];
        db[this.outputCollName].dropIndex(indexSpec);
    };

    /**
     * Changes the document validation options for the collection.
     */
    $config.states.collMod = function collMod(db, unusedCollName) {
        if (Random.rand() < 0.5) {
            // Change the validation level.
            const validationLevels = ['off', 'strict', 'moderate'];
            const newValidationLevel = validationLevels[Random.randInt(validationLevels.length)];
            assertWhenOwnDB.commandWorked(
                db.runCommand({collMod: this.outputCollName, validationLevel: newValidationLevel}));
        } else {
            // Change the validation action.
            assertWhenOwnDB.commandWorked(db.runCommand({
                collMod: this.outputCollName,
                validationAction: Random.rand() > 0.5 ? 'warn' : 'error'
            }));
        }
    };

    /**
     * Converts '$config.data.outputCollName' to a capped collection. This is never undone, and all
     * subsequent $out's to this collection should fail.
     */
    $config.states.convertToCapped = function convertToCapped(db, unusedCollName) {
        if (isMongos(db)) {
            return;  // convertToCapped can't be run against a mongos.
        }

        assertWhenOwnDB.commandWorked(
            db.runCommand({convertToCapped: this.outputCollName, size: 100000}));
    };

    /**
     * If being run against a mongos, shards '$config.data.outputCollName'. This is never undone,
     * and all subsequent $out's to this collection should fail.
     */
    $config.states.shardCollection = function shardCollection(db, unusedCollName) {
        if (isMongos(db)) {
            assertWhenOwnDB.commandWorked(db.adminCommand({enableSharding: db.getName()}));
            assertWhenOwnDB.commandWorked(db.adminCommand(
                {shardCollection: db[this.outputCollName].getFullName(), key: {_id: 'hashed'}}));
        }
    };

    /**
     * Calls the super class' setup but using our own database.
     */
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, [db, collName, cluster]);

        // `shardCollection()` requires a shard key index to be in place on the output collection,
        // as we may be sharding a non-empty collection.
        assertWhenOwnDB.commandWorked(db[this.outputCollName].createIndex({_id: 'hashed'}));
    };

    return $config;
});
