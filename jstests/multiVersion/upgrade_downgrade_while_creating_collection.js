/*
 * Tests that upgrade/downgrade works correctly even while creating a new collection.
 */
(function() {
    "use strict";
    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/libs/parallel_shell_helpers.js");

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();

    // Rig the election so that the first node is always primary and that modifying the
    // featureCompatibilityVersion document doesn't need to wait for data to replicate.
    var replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[1].priority = 0;
    replSetConfig.members[1].votes = 0;

    rst.initiate(replSetConfig);

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB("test");

    for (let versions
             of[{from: lastStableFCV, to: latestFCV}, {from: latestFCV, to: lastStableFCV}]) {
        jsTestLog("Changing FeatureCompatibilityVersion from " + versions.from + " to " +
                  versions.to + " while creating a collection");
        assert.commandWorked(
            primaryDB.adminCommand({setFeatureCompatibilityVersion: versions.from}));

        assert.commandWorked(primaryDB.adminCommand(
            {configureFailPoint: "hangBeforeLoggingCreateCollection", mode: "alwaysOn"}));
        primaryDB.mycoll.drop();

        let awaitCreateCollection;
        let awaitUpgradeFCV;

        try {
            awaitCreateCollection = startParallelShell(function() {
                assert.commandWorked(db.runCommand({create: "mycoll"}));
            }, primary.port);

            assert.soon(function() {
                return rawMongoProgramOutput().match("createCollection: test.mycoll");
            });

            awaitUpgradeFCV = startParallelShell(
                funWithArgs(function(version) {
                    assert.commandWorked(
                        db.adminCommand({setFeatureCompatibilityVersion: version}));
                }, versions.to), primary.port);

            {
                let res;
                assert.soon(
                    function() {
                        res = assert.commandWorked(primaryDB.adminCommand(
                            {getParameter: 1, featureCompatibilityVersion: 1}));
                        return res.featureCompatibilityVersion.version === versions.from &&
                            res.featureCompatibilityVersion.targetVersion === versions.new;
                    },
                    function() {
                        return "targetVersion of featureCompatibilityVersion document wasn't " +
                            "updated on primary: " + tojson(res);
                    });
            }
        } finally {
            assert.commandWorked(primaryDB.adminCommand(
                {configureFailPoint: "hangBeforeLoggingCreateCollection", mode: "off"}));
        }

        awaitCreateCollection();
        awaitUpgradeFCV();
        rst.checkReplicatedDataHashes();
    }
    rst.stopSet();
})();
