
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/config.h"

namespace mongo {

#if (MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_WINDOWS) || \
    (MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_APPLE)
#define MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS 1
#endif

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

struct SSLParams {
    enum class Protocols { TLS1_0, TLS1_1, TLS1_2, TLS1_3 };
    AtomicInt32 sslMode;            // --sslMode - the TLS operation mode, see enum SSLModes
    std::string sslPEMTempDHParam;  // --setParameter OpenSSLDiffieHellmanParameters=file : PEM file
                                    // with DH parameters.
    std::string sslPEMKeyFile;      // --sslPEMKeyFile
    std::string sslPEMKeyPassword;  // --sslPEMKeyPassword
    std::string sslClusterFile;     // --sslInternalKeyFile
    std::string sslClusterPassword;  // --sslInternalKeyPassword
    std::string sslCAFile;           // --sslCAFile
    std::string sslClusterCAFile;    // --sslClusterCAFile
    std::string sslCRLFile;          // --sslCRLFile
    std::string sslCipherConfig;     // --sslCipherConfig

    struct CertificateSelector {
        std::string subject;
        std::vector<uint8_t> thumbprint;
        bool empty() const {
            return subject.empty() && thumbprint.empty();
        }
    };
#ifdef MONGO_CONFIG_SSL_CERTIFICATE_SELECTORS
    CertificateSelector sslCertificateSelector;         // --sslCertificateSelector
    CertificateSelector sslClusterCertificateSelector;  // --sslClusterCertificateSelector
#endif

    std::vector<Protocols> sslDisabledProtocols;  // --sslDisabledProtocols
    std::vector<Protocols> tlsLogVersions;        // --tlsLogVersion
    bool sslWeakCertificateValidation = false;    // --sslWeakCertificateValidation
    bool sslFIPSMode = false;                     // --sslFIPSMode
    bool sslAllowInvalidCertificates = false;     // --sslAllowInvalidCertificates
    bool sslAllowInvalidHostnames = false;        // --sslAllowInvalidHostnames
    bool disableNonSSLConnectionLogging =
        false;  // --setParameter disableNonSSLConnectionLogging=true
    bool suppressNoTLSPeerCertificateWarning =
        false;  // --setParameter suppressNoTLSPeerCertificateWarning
    bool tlsWithholdClientCertificate = false;  // --setParameter tlsWithholdClientCertificate

    SSLParams() {
        sslMode.store(SSLMode_disabled);
    }

    enum SSLModes {
        /**
        * Make unencrypted outgoing connections and do not accept incoming SSL-connections.
        */
        SSLMode_disabled,

        /**
        * Make unencrypted outgoing connections and accept both unencrypted and SSL-connections.
        */
        SSLMode_allowSSL,

        /**
        * Make outgoing SSL-connections and accept both unecrypted and SSL-connections.
        */
        SSLMode_preferSSL,

        /**
        * Make outgoing SSL-connections and only accept incoming SSL-connections.
        */
        SSLMode_requireSSL
    };
};

extern SSLParams sslGlobalParams;

/**
* The global SSL configuration. This should be accessed only after global initialization has
* completed. If it must be accessed in an initializer, the initializer should have
* "EndStartupOptionStorage" as a prerequisite.
*/
const SSLParams& getSSLGlobalParams();

Status addSSLServerOptions(mongo::optionenvironment::OptionSection* options);

Status addSSLClientOptions(mongo::optionenvironment::OptionSection* options);

Status storeSSLServerOptions(const mongo::optionenvironment::Environment& params);

Status parseCertificateSelector(SSLParams::CertificateSelector* selector,
                                StringData name,
                                StringData value);
/**
 * Canonicalize SSL options for the given environment that have different representations with
 * the same logical meaning.
 */
Status canonicalizeSSLServerOptions(mongo::optionenvironment::Environment* params);

Status validateSSLServerOptions(const mongo::optionenvironment::Environment& params);

Status storeSSLClientOptions(const mongo::optionenvironment::Environment& params);
}  // namespace mongo
