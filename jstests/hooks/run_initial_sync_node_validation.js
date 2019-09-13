// Runner that runs full validation on all collections of the initial sync node and checks the
// dbhashes of all of the nodes including the initial sync node.
'use strict';

(function() {
    var startTime = Date.now();

    var primaryInfo = db.isMaster();
    assert(primaryInfo.ismaster,
           'shell is not connected to the primary node: ' + tojson(primaryInfo));

    var cmdLineOpts = db.adminCommand('getCmdLineOpts');
    assert.commandWorked(cmdLineOpts);

    // The initial sync hooks only work for replica sets.
    var rst = new ReplSetTest(db.getMongo().host);

    // Call getPrimary to populate rst with information about the nodes.
    var primary = rst.getPrimary();
    assert(primary, 'calling getPrimary() failed');

    // Find the hidden node.
    var hiddenNode;
    for (var secondary of rst._slaves) {
        var isMasterRes = secondary.getDB('admin').isMaster();
        if (isMasterRes.hidden) {
            hiddenNode = secondary;
            break;
        }
    }

    assert(hiddenNode, 'No hidden initial sync node was found in the replica set');

    // Confirm that the hidden node is in SECONDARY state.
    var res = assert.commandWorked(hiddenNode.adminCommand({replSetGetStatus: 1}));
    assert.eq(res.myState, ReplSetTest.State.SECONDARY, tojson(res));

    /* The checkReplicatedDataHashes call waits until all operations have replicated to and
       have been applied on the secondaries, so we run the validation script after it
       to ensure we're validating the entire contents of the collection */

    // For checkDBHashes
    const excludedDBs = jsTest.options().excludedDBsFromDBHash;
    rst.checkReplicatedDataHashes(undefined, excludedDBs);

    load('jstests/hooks/run_validate_collections.js');

    var totalTime = Date.now() - startTime;
    print('Finished consistency checks of initial sync node in ' + totalTime + ' ms.');
})();
