// Test that including intermediate certificates
// in the certificate key file will be sent to the remote.

(function() {
    'use strict';

    load('jstests/ssl/libs/ssl_helpers.js');

    // server-intermediate-ca was signed by ca.pem, not trusted-ca.pem
    const VALID_CA = 'jstests/libs/ca.pem';
    const INVALID_CA = 'jstests/libs/trusted-ca.pem';

    function runTest(inbound, outbound) {
        const mongod = MongoRunner.runMongod({
            sslMode: 'requireSSL',
            sslAllowConnectionsWithoutCertificates: '',
            sslPEMKeyFile: 'jstests/libs/server-intermediate-ca.pem',
            sslCAFile: outbound,
            sslClusterCAFile: inbound,
        });
        assert(mongod);
        assert.eq(mongod.getDB('admin').system.users.find({}).toArray(), []);
        MongoRunner.stopMongod(mongod);
    }

    // Normal mode, we have a valid CA being presented for outbound and inbound.
    runTest(VALID_CA, VALID_CA);

    // Alternate CA mode, only the inbound CA is valid.
    runTest(VALID_CA, INVALID_CA);

    // Validate we can make a connection from the shell with the intermediate certs
    {
        const mongod = MongoRunner.runMongod({
            sslMode: 'requireSSL',
            sslAllowConnectionsWithoutCertificates: '',
            sslPEMKeyFile: 'jstests/libs/server.pem',
            sslCAFile: VALID_CA,
        });
        assert(mongod);
        assert.eq(mongod.getDB('admin').system.users.find({}).toArray(), []);

        const smoke = runMongoProgram("mongo",
                                      "--host",
                                      "localhost",
                                      "--port",
                                      mongod.port,
                                      "--ssl",
                                      "--sslCAFile",
                                      VALID_CA,
                                      "--sslPEMKeyFile",
                                      "jstests/libs/server-intermediate-ca.pem",
                                      "--eval",
                                      "1;");
        assert.eq(smoke, 0, "Could not connect with intermediate certificate");

        MongoRunner.stopMongod(mongod);
    }
})();
