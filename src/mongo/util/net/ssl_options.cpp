/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/util/net/ssl_options.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/base/status.h"
#include "mongo/db/server_options.h"
#include "mongo/util/text.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    Status addSSLServerOptions(moe::OptionSection* options) {
        options->addOptionChaining("net.ssl.sslOnNormalPorts", "sslOnNormalPorts", moe::Switch,
                "use ssl on configured ports")
                                  .setSources(moe::SourceAllLegacy)
                                  .incompatibleWith("net.ssl.mode");

        options->addOptionChaining("net.ssl.mode", "sslMode", moe::String,
                "set the SSL operation mode (disabled|allowSSL|preferSSL|requireSSL)");

        options->addOptionChaining("net.ssl.PEMKeyFile", "sslPEMKeyFile", moe::String,
                "PEM file for ssl");

        options->addOptionChaining("net.ssl.PEMKeyPassword", "sslPEMKeyPassword", moe::String,
                "PEM file password")
                                  .setImplicit(moe::Value(std::string("")));

        options->addOptionChaining("net.ssl.clusterFile", "sslClusterFile", moe::String,
                "Key file for internal SSL authentication");

        options->addOptionChaining("net.ssl.clusterPassword", "sslClusterPassword", moe::String,
                "Internal authentication key file password")
                                  .setImplicit(moe::Value(std::string("")));

        options->addOptionChaining("net.ssl.CAFile", "sslCAFile", moe::String,
                "Certificate Authority file for SSL");

        options->addOptionChaining("net.ssl.CRLFile", "sslCRLFile", moe::String,
                "Certificate Revocation List file for SSL");

        options->addOptionChaining("net.ssl.sslCipherConfig", "sslCipherConfig", moe::String,
                "OpenSSL cipher configuration string")
                                   .hidden();

        options->addOptionChaining("net.ssl.disabledProtocols", "sslDisabledProtocols", moe::String,
                "Comma separated list of TLS protocols to disable [TLS1_0,TLS1_1,TLS1_2]");

        options->addOptionChaining("net.ssl.weakCertificateValidation",
                "sslWeakCertificateValidation", moe::Switch, "allow client to connect without "
                "presenting a certificate");

        options->addOptionChaining("net.ssl.allowInvalidHostnames", "sslAllowInvalidHostnames",
                moe::Switch, "Allow server certificates to provide non-matching hostnames");

        options->addOptionChaining("net.ssl.allowInvalidCertificates", "sslAllowInvalidCertificates",
                    moe::Switch, "allow connections to servers with invalid certificates");

        options->addOptionChaining("net.ssl.FIPSMode", "sslFIPSMode", moe::Switch,
                "activate FIPS 140-2 mode at startup");

        return Status::OK();
    }

    Status addSSLClientOptions(moe::OptionSection* options) {
        options->addOptionChaining("ssl", "ssl", moe::Switch, "use SSL for all connections");

        options->addOptionChaining("ssl.CAFile", "sslCAFile", moe::String,
                "Certificate Authority file for SSL")
                                  .requires("ssl");

        options->addOptionChaining("ssl.PEMKeyFile", "sslPEMKeyFile", moe::String,
                "PEM certificate/key file for SSL")
                                  .requires("ssl");

        options->addOptionChaining("ssl.PEMKeyPassword", "sslPEMKeyPassword", moe::String,
                "password for key in PEM file for SSL")
                                  .requires("ssl");

        options->addOptionChaining("ssl.CRLFile", "sslCRLFile", moe::String,
                "Certificate Revocation List file for SSL")
                                  .requires("ssl")
                                  .requires("ssl.CAFile");

        options->addOptionChaining("net.ssl.allowInvalidHostnames", "sslAllowInvalidHostnames",
                    moe::Switch, "allow connections to servers with non-matching hostnames")
                                  .requires("ssl");

        options->addOptionChaining("ssl.allowInvalidCertificates", "sslAllowInvalidCertificates",
                    moe::Switch, "allow connections to servers with invalid certificates")
                                  .requires("ssl");

        options->addOptionChaining("ssl.FIPSMode", "sslFIPSMode", moe::Switch,
                "activate FIPS 140-2 mode at startup")
                                  .requires("ssl");

        return Status::OK();
    }

    Status canonicalizeSSLServerOptions(moe::Environment* params) {

        if (params->count("net.ssl.sslOnNormalPorts")) {
            Status ret = params->set("net.ssl.mode", moe::Value(std::string("requireSSL")));
            if (!ret.isOK()) {
                return ret;
            }
            ret = params->remove("net.ssl.sslOnNormalPorts");
            if (!ret.isOK()) {
                return ret;
            }
        }

        return Status::OK();
    }

    Status storeSSLServerOptions(const moe::Environment& params) {

        if (params.count("net.ssl.mode")) {
            std::string sslModeParam = params["net.ssl.mode"].as<string>();
            if (sslModeParam == "disabled") {
                sslGlobalParams.sslMode.store(SSLGlobalParams::SSLMode_disabled);
            }
            else if (sslModeParam == "allowSSL") {
                sslGlobalParams.sslMode.store(SSLGlobalParams::SSLMode_allowSSL);
            }
            else if (sslModeParam == "preferSSL") {
                sslGlobalParams.sslMode.store(SSLGlobalParams::SSLMode_preferSSL);
            }
            else if (sslModeParam == "requireSSL") {
                sslGlobalParams.sslMode.store(SSLGlobalParams::SSLMode_requireSSL);
            }
            else {
                return Status(ErrorCodes::BadValue,
                              "unsupported value for sslMode " + sslModeParam );
            }
        }

        if (params.count("net.ssl.PEMKeyFile")) {
            sslGlobalParams.sslPEMKeyFile = boost::filesystem::absolute(
                                        params["net.ssl.PEMKeyFile"].as<string>()).generic_string();
        }

        if (params.count("net.ssl.PEMKeyPassword")) {
            sslGlobalParams.sslPEMKeyPassword = params["net.ssl.PEMKeyPassword"].as<string>();
        }

        if (params.count("net.ssl.clusterFile")) {
            sslGlobalParams.sslClusterFile =
                boost::filesystem::absolute(
                        params["net.ssl.clusterFile"].as<string>()).generic_string();
        }

        if (params.count("net.ssl.clusterPassword")) {
            sslGlobalParams.sslClusterPassword = params["net.ssl.clusterPassword"].as<string>();
        }

        if (params.count("net.ssl.CAFile")) {
            sslGlobalParams.sslCAFile = boost::filesystem::absolute(
                                         params["net.ssl.CAFile"].as<std::string>()).generic_string();
        }

        if (params.count("net.ssl.CRLFile")) {
            sslGlobalParams.sslCRLFile = boost::filesystem::absolute(
                                         params["net.ssl.CRLFile"].as<std::string>()).generic_string();
        }

        if (params.count("net.ssl.sslCipherConfig")) {
            sslGlobalParams.sslCipherConfig = params["net.ssl.sslCipherConfig"].as<string>();
        }

        if (params.count("net.ssl.disabledProtocols")) {
            // The disabledProtocols field is composed of a comma separated list of protocols to
            // disable. First, tokenize the field.
            std::vector<std::string> tokens = StringSplitter::split(
                    params["net.ssl.disabledProtocols"].as<string>(), ",");

            // All accepted tokens, and their corresponding enum representation. The noTLS* tokens
            // exist for backwards compatibility.
            std::map<std::string, SSLGlobalParams::Protocols> validConfigs;
            validConfigs["TLS1_0"] = SSLGlobalParams::TLS1_0;
            validConfigs["noTLS1_0"] = SSLGlobalParams::TLS1_0;
            validConfigs["TLS1_1"] = SSLGlobalParams::TLS1_1;
            validConfigs["noTLS1_1"] = SSLGlobalParams::TLS1_1;
            validConfigs["TLS1_2"] = SSLGlobalParams::TLS1_2;
            validConfigs["noTLS1_2"] = SSLGlobalParams::TLS1_2;

            // Map the tokens to their enum values, and push them onto the list of disabled protocols.
            for (std::vector<std::string>::iterator it = tokens.begin(); it != tokens.end(); ++it) {
                std::map<std::string, SSLGlobalParams::Protocols>::iterator mappedToken =
                    validConfigs.find(*it);
                if (mappedToken != validConfigs.end()) {
                    sslGlobalParams.sslDisabledProtocols.push_back(mappedToken->second);
                } else {
                    return Status(ErrorCodes::BadValue,
                                  "Unrecognized disabledProtocols '" + *it +"'");
                }
            }
        }

        if (params.count("net.ssl.weakCertificateValidation")) {
            sslGlobalParams.sslWeakCertificateValidation = true;
        }
        if (params.count("net.ssl.allowInvalidHostnames")) {
            sslGlobalParams.sslAllowInvalidHostnames =
                params["net.ssl.allowInvalidHostnames"].as<bool>();
        }
        if (params.count("net.ssl.allowInvalidCertificates")) {
            sslGlobalParams.sslAllowInvalidCertificates = true;
        }
        if (params.count("net.ssl.FIPSMode")) {
            sslGlobalParams.sslFIPSMode = true;
        }

        if (sslGlobalParams.sslMode.load() != SSLGlobalParams::SSLMode_disabled) {
            if (sslGlobalParams.sslPEMKeyFile.size() == 0) {
                return Status(ErrorCodes::BadValue,
                              "need sslPEMKeyFile when SSL is enabled");
            }
            if (sslGlobalParams.sslWeakCertificateValidation &&
                sslGlobalParams.sslCAFile.empty()) {
                return Status(ErrorCodes::BadValue,
                              "need sslCAFile with sslWeakCertificateValidation");
            }
            if (!sslGlobalParams.sslCRLFile.empty() &&
                sslGlobalParams.sslCAFile.empty()) {
                return Status(ErrorCodes::BadValue, "need sslCAFile with sslCRLFile");
            }
            if (sslGlobalParams.sslCAFile.empty()) {
                warning() << "No SSL certificate validation can be performed since no CA file "
                             "has been provided; please specify an sslCAFile parameter";
            }
        }
        else if (sslGlobalParams.sslPEMKeyFile.size() ||
                 sslGlobalParams.sslPEMKeyPassword.size() ||
                 sslGlobalParams.sslClusterFile.size() ||
                 sslGlobalParams.sslClusterPassword.size() ||
                 sslGlobalParams.sslCAFile.size() ||
                 sslGlobalParams.sslCRLFile.size() ||
                 sslGlobalParams.sslCipherConfig.size() ||
                 sslGlobalParams.sslDisabledProtocols.size() ||
                 sslGlobalParams.sslWeakCertificateValidation ||
                 sslGlobalParams.sslFIPSMode) {
            return Status(ErrorCodes::BadValue,
                          "need to enable SSL via the sslMode flag when "
                          "using SSL configuration parameters");
        }
        int clusterAuthMode = serverGlobalParams.clusterAuthMode.load(); 
        if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendKeyFile ||
            clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendX509 ||
            clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509) {
            if (sslGlobalParams.sslMode.load() == SSLGlobalParams::SSLMode_disabled) {
                return Status(ErrorCodes::BadValue, "need to enable SSL via the sslMode flag");
            } 
        }
        if (sslGlobalParams.sslMode.load() == SSLGlobalParams::SSLMode_allowSSL) {
            if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendX509 ||
                clusterAuthMode == ServerGlobalParams::ClusterAuthMode_x509) {
                    return Status(ErrorCodes::BadValue,
                                  "cannot have x.509 cluster authentication in allowSSL mode");
            }
        }
        return Status::OK();
    }

    Status storeSSLClientOptions(const moe::Environment& params) {
        if (params.count("ssl")) {
            sslGlobalParams.sslMode.store(SSLGlobalParams::SSLMode_requireSSL);
        }
        if (params.count("ssl.PEMKeyFile")) {
            sslGlobalParams.sslPEMKeyFile = params["ssl.PEMKeyFile"].as<std::string>();
        }
        if (params.count("ssl.PEMKeyPassword")) {
            sslGlobalParams.sslPEMKeyPassword = params["ssl.PEMKeyPassword"].as<std::string>();
        }
        if (params.count("ssl.CAFile")) {
            sslGlobalParams.sslCAFile = params["ssl.CAFile"].as<std::string>();
        }
        if (params.count("ssl.CRLFile")) {
            sslGlobalParams.sslCRLFile = params["ssl.CRLFile"].as<std::string>();
        }
        if (params.count("net.ssl.allowInvalidHostnames")) {
            sslGlobalParams.sslAllowInvalidHostnames =
                params["net.ssl.allowInvalidHostnames"].as<bool>();
        }
        if (params.count("ssl.allowInvalidCertificates")) {
            sslGlobalParams.sslAllowInvalidCertificates = true;
        }
        if (params.count("ssl.FIPSMode")) {
            sslGlobalParams.sslFIPSMode = true;
        }
        return Status::OK();
    }

} // namespace mongo
