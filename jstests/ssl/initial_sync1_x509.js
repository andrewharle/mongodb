// Basic tests for cluster authentication using x509.

var common_options = {
    keyFile: "jstests/libs/key1",
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem",
    sslAllowInvalidHostnames: ""
};

function runInitialSyncTest() {
    load("jstests/replsets/rslib.js");

    // The mongo shell cannot authenticate as the internal __system user in tests that use x509 for
    // cluster authentication. Choosing the default value for wcMajorityJournalDefault in
    // ReplSetTest cannot be done automatically without the shell performing such authentication, so
    // in this test we must make the choice explicitly, based on the global test options.
    var wcMajorityJournalDefault;
    if (jsTestOptions().noJournal || jsTestOptions().storageEngine == "ephemeralForTest" ||
        jsTestOptions().storageEngine == "inMemory") {
        wcMajorityJournalDefault = false;
    } else {
        wcMajorityJournalDefault = true;
    }
    print("1. Bring up set");
    var replTest = new ReplSetTest({
        name: "jstests_initsync1_x509",
        nodes: {node0: x509_options1, node1: x509_options2},
        waitForKeys: false
    });
    var conns = replTest.startSet();

    // ReplSetTest.initiate() requires all nodes to be to be authorized to run replSetGetStatus.
    // TODO(SERVER-14017): Remove this in favor of using initiate() everywhere.
    replTest.initiateWithAnyNodeAsPrimary(
        Object.extend(replTest.getReplSetConfig(),
                      {writeConcernMajorityJournalDefault: wcMajorityJournalDefault}));

    var master = replTest.getPrimary();
    var foo = master.getDB("foo");
    var admin = master.getDB("admin");

    var slave1 = replTest._slaves[0];
    var admin_s1 = slave1.getDB("admin");

    print("2. Create a root user.");
    admin.createUser({user: "root", pwd: "pass", roles: ["root"]});
    admin.auth("root", "pass");
    admin_s1.auth("root", "pass");

    print("3. Insert some data");
    var bulk = foo.bar.initializeUnorderedBulkOp();
    for (var i = 0; i < 100; i++) {
        bulk.insert({date: new Date(), x: i, str: "all the talk on the market"});
    }
    assert.writeOK(bulk.execute());
    print("total in foo: " + foo.bar.count());

    print("4. Make sure synced");
    replTest.awaitReplication();

    print("5. Insert some stuff");
    master = replTest.getPrimary();
    bulk = foo.bar.initializeUnorderedBulkOp();
    for (var i = 0; i < 100; i++) {
        bulk.insert({date: new Date(), x: i, str: "all the talk on the market"});
    }
    assert.writeOK(bulk.execute());

    print("6. Everyone happy eventually");
    replTest.awaitReplication(300000);

    replTest.stopSet();
}

// Standard case, clusterAuthMode: x509
var x509_options1 = Object.merge(
    common_options, {sslClusterFile: "jstests/libs/cluster_cert.pem", clusterAuthMode: "x509"});
var x509_options2 = x509_options1;
runInitialSyncTest();

// Mixed clusterAuthMode: sendX509 and sendKeyFile and try adding --auth
x509_options1 = Object.merge(
    common_options,
    {sslClusterFile: "jstests/libs/cluster_cert.pem", clusterAuthMode: "sendX509", auth: ""});
x509_options2 = Object.merge(common_options, {clusterAuthMode: "sendKeyFile"});
runInitialSyncTest();

// Mixed clusterAuthMode: x509 and sendX509, use the PEMKeyFile for outgoing connections
x509_options1 = Object.merge(common_options, {clusterAuthMode: "x509"});
x509_options2 = Object.merge(common_options, {clusterAuthMode: "sendX509"});
runInitialSyncTest();

// verify that replset initiate fails if using a self-signed cert
x509_options1 = Object.merge(common_options, {clusterAuthMode: "x509"});
x509_options2 = Object.merge(common_options,
                             {sslClusterFile: "jstests/libs/smoke.pem", clusterAuthMode: "x509"});
var replTest = new ReplSetTest({nodes: {node0: x509_options1, node1: x509_options2}});
var conns = replTest.startSet();
assert.throws(function() {
    replTest.initiate();
});
replTest.stopSet();
