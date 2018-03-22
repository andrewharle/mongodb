/**
 * Use prototype overrides to set read preference to "secondary" when running tests.
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");

    const kReadPreferenceSecondary = {mode: "secondary"};
    const kCommandsSupportingReadPreference = new Set([
        "aggregate",
        "collStats",
        "count",
        "dbStats",
        "distinct",
        "find",
        "geoNear",
        "geoSearch",
        "group",
        "mapReduce",
        "mapreduce",
        "parallelCollectionScan",
    ]);

    // This list of cursor-generating commands is incomplete. For example, "listCollections",
    // "listIndexes", "parallelCollectionScan", and "repairCursor" are all missing from this list.
    // If we ever add tests that attempt to run getMore or killCursors on cursors generated from
    // those commands, then we should update the contents of this list and also handle any
    // differences in the server's response format.
    const kCursorGeneratingCommands = new Set(["aggregate", "find"]);

    const CursorTracker = (function() {
        const kNoCursor = new NumberLong(0);

        const connectionsByCursorId = {};

        return {
            getConnectionUsedForCursor: function getConnectionUsedForCursor(cursorId) {
                return (cursorId instanceof NumberLong) ? connectionsByCursorId[cursorId]
                                                        : undefined;
            },

            setConnectionUsedForCursor: function setConnectionUsedForCursor(cursorId, cursorConn) {
                if (cursorId instanceof NumberLong &&
                    !bsonBinaryEqual({_: cursorId}, {_: kNoCursor})) {
                    connectionsByCursorId[cursorId] = cursorConn;
                }
            },
        };
    })();

    function runCommandWithReadPreferenceSecondary(
        conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        if (typeof commandObj !== "object" || commandObj === null) {
            return func.apply(conn, makeFuncArgs(commandObj));
        }

        // If the command is in a wrapped form, then we look for the actual command object inside
        // the query/$query object.
        let commandObjUnwrapped = commandObj;
        if (commandName === "query" || commandName === "$query") {
            commandObjUnwrapped = commandObj[commandName];
            commandName = Object.keys(commandObjUnwrapped)[0];
        }

        if (commandObj[commandName] === "system.profile") {
            throw new Error("Cowardly refusing to run test with overridden read preference" +
                            " when it reads from a non-replicated collection: " +
                            tojson(commandObj));
        }

        if (conn.isReplicaSetConnection()) {
            // When a "getMore" or "killCursors" command is issued on a replica set connection, we
            // attempt to automatically route the command to the server the cursor(s) were
            // originally established on. This makes it possible to use the
            // set_read_preference_secondary.js override without needing to update calls of
            // DB#runCommand() to explicitly track the connection that was used. If the connection
            // is actually a direct connection to a mongod or mongos process, or if the cursor id
            // cannot be found in the CursorTracker, then we'll fall back to using DBClientRS's
            // server selection and send the operation to the current primary. It is possible that
            // the test is trying to exercise the behavior around when an unknown cursor id is sent
            // to the server.
            if (commandName === "getMore") {
                const cursorId = commandObjUnwrapped[commandName];
                const cursorConn = CursorTracker.getConnectionUsedForCursor(cursorId);
                if (cursorConn !== undefined) {
                    return func.apply(cursorConn, makeFuncArgs(commandObj));
                }
            } else if (commandName === "killCursors") {
                const cursorIds = commandObjUnwrapped.cursors;
                if (Array.isArray(cursorIds)) {
                    let cursorConn;

                    for (let cursorId of cursorIds) {
                        const otherCursorConn = CursorTracker.getConnectionUsedForCursor(cursorId);
                        if (cursorConn === undefined) {
                            cursorConn = otherCursorConn;
                        } else if (otherCursorConn !== undefined) {
                            // We set 'cursorConn' back to undefined and break out of the loop so
                            // that we don't attempt to automatically route the "killCursors"
                            // command when there are cursors from different servers.
                            cursorConn = undefined;
                            break;
                        }
                    }

                    if (cursorConn !== undefined) {
                        return func.apply(cursorConn, makeFuncArgs(commandObj));
                    }
                }
            }
        }

        let shouldForceReadPreference = kCommandsSupportingReadPreference.has(commandName);
        if (OverrideHelpers.isAggregationWithOutStage(commandName, commandObjUnwrapped)) {
            // An aggregation with a $out stage must be sent to the primary.
            shouldForceReadPreference = false;
        } else if ((commandName === "mapReduce" || commandName === "mapreduce") &&
                   !OverrideHelpers.isMapReduceWithInlineOutput(commandName, commandObjUnwrapped)) {
            // A map-reduce operation with non-inline output must be sent to the primary.
            shouldForceReadPreference = false;
        }

        if (shouldForceReadPreference) {
            if (commandObj === commandObjUnwrapped) {
                // We wrap the command object using a "query" field rather than a "$query" field to
                // match the implementation of DB.prototype._attachReadPreferenceToCommand().
                commandObj = {query: commandObj};
            } else {
                // We create a copy of 'commandObj' to avoid mutating the parameter the caller
                // specified.
                commandObj = Object.assign({}, commandObj);
            }

            if (commandObj.hasOwnProperty("$readPreference") &&
                !bsonBinaryEqual({_: commandObj.$readPreference}, {_: kReadPreferenceSecondary})) {
                throw new Error("Cowardly refusing to override read preference of command: " +
                                tojson(commandObj));
            }

            commandObj.$readPreference = kReadPreferenceSecondary;
        }

        const serverResponse = func.apply(conn, makeFuncArgs(commandObj));

        if (conn.isReplicaSetConnection() && kCursorGeneratingCommands.has(commandName) &&
            serverResponse.ok === 1 && serverResponse.hasOwnProperty("cursor")) {
            // We associate the cursor id returned by the server with the connection that was used
            // to establish it so that we can attempt to automatically route subsequent "getMore"
            // and "killCursors" commands.
            CursorTracker.setConnectionUsedForCursor(serverResponse.cursor.id,
                                                     serverResponse._mongo);
        }

        return serverResponse;
    }

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/set_read_preference_secondary.js");

    OverrideHelpers.overrideRunCommand(runCommandWithReadPreferenceSecondary);
})();
