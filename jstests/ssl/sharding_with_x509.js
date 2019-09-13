// Tests basic sharding with x509 cluster auth. The purpose is to verify the connectivity between
// mongos and the shards.
(function() {
    'use strict';

    var x509_options = {
        sslMode: "requireSSL",
        sslPEMKeyFile: "jstests/libs/server.pem",
        sslCAFile: "jstests/libs/ca.pem",
        sslClusterFile: "jstests/libs/cluster_cert.pem",
        sslAllowInvalidHostnames: "",
        clusterAuthMode: "x509"
    };

    // Start ShardingTest with enableBalancer because ShardingTest attempts to turn off the balancer
    // otherwise, which it will not be authorized to do. Once SERVER-14017 is fixed the
    // "enableBalancer" line could be removed.
    // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
    var st = new ShardingTest({
        shards: 2,
        mongos: 1,
        other: {
            enableBalancer: true,
            configOptions: x509_options,
            mongosOptions: x509_options,
            rsOptions: x509_options,
            shardOptions: x509_options,
            shardAsReplicaSet: false
        }
    });

    st.s.getDB('admin').createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
    st.s.getDB('admin').auth('admin', 'pwd');

    var coll = st.s.getCollection("test.foo");

    st.shardColl(coll, {insert: 1}, false);

    print("starting insertion phase");

    // Insert a bunch of data
    var toInsert = 2000;
    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < toInsert; i++) {
        bulk.insert({my: "test", data: "to", insert: i});
    }
    assert.writeOK(bulk.execute());

    print("starting updating phase");

    // Update a bunch of data
    var toUpdate = toInsert;
    bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < toUpdate; i++) {
        var id = coll.findOne({insert: i})._id;
        bulk.find({insert: i, _id: id}).update({$inc: {counter: 1}});
    }
    assert.writeOK(bulk.execute());

    print("starting deletion");

    // Remove a bunch of data
    var toDelete = toInsert / 2;
    bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < toDelete; i++) {
        bulk.find({insert: i}).removeOne();
    }
    assert.writeOK(bulk.execute());

    // Make sure the right amount of data is there
    assert.eq(coll.find().itcount({my: 'test'}), toInsert / 2);

    // Authenticate csrs so ReplSetTest.stopSet() can do db hash check.
    if (st.configRS) {
        st.configRS.nodes.forEach((node) => {
            node.getDB('admin').auth('admin', 'pwd');
        });
    }
    st.stop();
})();
