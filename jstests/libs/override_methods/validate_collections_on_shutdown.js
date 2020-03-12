/**
 * Load this file when starting a mongo shell program in order to provide a callback to validate
 * collections and indexes before shutting down a mongod while running JS tests.
 */

(function() {
    "use strict";

    load("jstests/libs/command_sequence_with_retries.js");  // for CommandSequenceWithRetries

    MongoRunner.validateCollectionsCallback = function(port) {
        if (jsTest.options().skipCollectionAndIndexValidation) {
            print("Skipping collection validation during mongod shutdown");
            return;
        }

        let conn;
        try {
            conn = new Mongo("localhost:" + port);
        } catch (e) {
            print(
                "Skipping collection validation because we couldn't establish a connection to the" +
                " server on port " + port);
            return;
        }

        // Set slaveOk=true so that we can run commands against any secondaries.
        conn.setSlaveOk();

        let dbNames;
        let result =
            new CommandSequenceWithRetries(conn)
                .then("running the isMaster command",
                      function(conn) {
                          const res = assert.commandWorked(conn.adminCommand({isMaster: 1}));
                          if (res.msg === "isdbgrid") {
                              return {
                                  shouldStop: true,
                                  reason: "not running validate against mongos"
                              };
                          } else if (!res.ismaster && !res.secondary) {
                              return {
                                  shouldStop: true,
                                  reason: "not running validate since mongod isn't in the PRIMARY" +
                                      " or SECONDARY states"
                              };
                          }
                      })
                .then("authenticating",
                      function(conn) {
                          if (jsTest.options().keyFile) {
                              jsTest.authenticate(conn);
                          }
                      })
                .then(
                    "best effort to step down node forever",
                    function(conn) {
                        if (conn.isReplicaSetMember()) {
                            // This node should never run for election again. If the node has not
                            // been initialized yet, then it cannot get elected.
                            const kFreezeTimeSecs = 24 * 60 * 60;  // 24 hours.

                            assert.soon(
                                () => {
                                    assert.commandWorkedOrFailedWithCode(
                                        conn.adminCommand(
                                            {replSetStepDown: kFreezeTimeSecs, force: true}),
                                        [
                                          ErrorCodes.NotMaster,
                                          ErrorCodes.NotYetInitialized,
                                          ErrorCodes.Unauthorized
                                        ]);
                                    const res = conn.adminCommand({replSetFreeze: kFreezeTimeSecs});
                                    assert.commandWorkedOrFailedWithCode(res, [
                                        ErrorCodes.NotYetInitialized,
                                        ErrorCodes.Unauthorized,
                                        ErrorCodes.NotSecondary
                                    ]);

                                    // If 'replSetFreeze' succeeds or fails with NotYetInitialized
                                    // or Unauthorized, we do not need to retry the command because
                                    // retrying will not work if the replica set is not yet
                                    // initialized or if we are not authorized to run the command.
                                    // This is why this is a "best-effort".
                                    if (res.ok === 1 || res.code !== ErrorCodes.NotSecondary) {
                                        return true;
                                    }

                                    // We only retry on NotSecondary error because 'replSetFreeze'
                                    // could fail with NotSecondary if the node is currently primary
                                    // or running for election. This could happen if there is a
                                    // concurrent election running in parallel with the
                                    // 'replSetStepDown' sent above.
                                    jsTestLog(
                                        "Retrying 'replSetStepDown' and 'replSetFreeze' in port " +
                                        conn.port + " res: " + tojson(res));
                                    return false;
                                },
                                "Timed out running 'replSetStepDown' and 'replSetFreeze' node in " +
                                    "port " + conn.port);
                        }
                    })
                .then("getting the list of databases",
                      function(conn) {
                          const res = conn.adminCommand({listDatabases: 1});
                          if (!res.ok) {
                              // TODO: SERVER-31916 for the KeyNotFound error
                              assert.commandFailedWithCode(
                                  res, [ErrorCodes.Unauthorized, ErrorCodes.KeyNotFound]);
                              return {shouldStop: true, reason: "cannot run listDatabases"};
                          }
                          assert.commandWorked(res);
                          dbNames = res.databases.map(dbInfo => dbInfo.name);
                      })
                .execute();

        if (!result.ok) {
            print("Skipping collection validation: " + result.msg);
            return;
        }

        load('jstests/hooks/validate_collections.js');  // for validateCollections

        const cmds = new CommandSequenceWithRetries(conn);
        for (let i = 0; i < dbNames.length; ++i) {
            const dbName = dbNames[i];
            cmds.then("validating " + dbName, function(conn) {
                if (!validateCollections(conn.getDB(dbName), {full: true})) {
                    return {shouldStop: true, reason: "collection validation failed"};
                }
            });
        }

        result = cmds.execute();
        if (!result.ok) {
            throw new Error(result.reason);
        }
    };
})();
