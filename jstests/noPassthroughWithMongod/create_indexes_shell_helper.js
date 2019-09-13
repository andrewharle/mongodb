(function() {
    "use strict";
    var t = db.create_indexes_shell_helper;
    t.drop();

    var mongo = db.getMongo();

    try {
        var commandsRan = [];
        var insertsRan = [];
        var mockMongo = {
            forceWriteMode: function(mode) {
                this._writeMode = mode;
            },
            writeMode: function() {
                return this._writeMode;
            },
            getSlaveOk: function() {
                return true;
            },
            runCommand: function(db, cmd, opts) {
                commandsRan.push({db: db, cmd: cmd, opts: opts});
                return {ok: 1.0};
            },
            insert: function(db, indexSpecs, opts) {
                insertsRan.push({db: db, indexSpecs: indexSpecs, opts: opts});
                return {ok: 1.0};
            },
            getWriteConcern: function() {
                return null;
            },
            useWriteCommands: function() {
                return true;
            },
            hasWriteCommands: function() {
                return true;
            },
            getMinWireVersion: function() {
                return mongo.getMinWireVersion();
            },
            getMaxWireVersion: function() {
                return mongo.getMaxWireVersion();
            },
            isReplicaSetMember: function() {
                return mongo.isReplicaSetMember();
            },
            isMongos: function() {
                return mongo.isMongos();
            },
            isCausalConsistency: function() {
                return false;
            },
            getClusterTime: function() {
                return null;
            },
        };

        db._mongo = mockMongo;
        db._session = new _DummyDriverSession(mockMongo);

        mockMongo.forceWriteMode("commands");

        t.createIndexes([{x: 1}]);
        assert.eq(commandsRan.length, 1);
        assert(commandsRan[0].cmd.hasOwnProperty("createIndexes"));
        assert.eq(commandsRan[0].cmd["indexes"][0], {key: {x: 1}, name: "x_1"});

        commandsRan = [];

        t.createIndexes([{y: 1}, {z: -1}]);
        assert.eq(commandsRan.length, 1);
        assert(commandsRan[0].cmd.hasOwnProperty("createIndexes"));
        assert.eq(commandsRan[0].cmd["indexes"][0], {key: {y: 1}, name: "y_1"});
        assert.eq(commandsRan[0].cmd["indexes"][1], {key: {z: -1}, name: "z_-1"});

        commandsRan = [];

        t.createIndex({a: 1});
        assert.eq(commandsRan.length, 1);
        assert(commandsRan[0].cmd.hasOwnProperty("createIndexes"));
        assert.eq(commandsRan[0].cmd["indexes"][0], {key: {a: 1}, name: "a_1"});

        db.getMongo().forceWriteMode("compatibility");

        commandsRan = [];
        assert.eq(commandsRan.length, 0);
        t.createIndex({b: 1});
        assert.eq(insertsRan.length, 1);
        assert.eq(insertsRan[0]["indexSpecs"]["ns"], "test.create_indexes_shell_helper");
        assert.eq(insertsRan[0]["indexSpecs"]["key"], {b: 1});
        assert.eq(insertsRan[0]["indexSpecs"]["name"], "b_1");
        // getLastError is called in the course of the bulk insert
        assert.eq(commandsRan.length, 1);
        assert(commandsRan[0].cmd.hasOwnProperty("getlasterror"));
    } finally {
        db._mongo = mongo;
        db._session = new _DummyDriverSession(mongo);
    }
}());
