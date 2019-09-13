
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/mongos_options.h"

#include <iostream>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_options_server_helpers.h"
#include "mongo/s/version_mongos.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/stringutils.h"

namespace mongo {

MongosGlobalParams mongosGlobalParams;

Status addMongosOptions(moe::OptionSection* options) {
    moe::OptionSection general_options("General options");

    Status ret = addGeneralServerOptions(&general_options);
    if (!ret.isOK()) {
        return ret;
    }

#if defined(_WIN32)
    moe::OptionSection windows_scm_options("Windows Service Control Manager options");

    ret = addWindowsServerOptions(&windows_scm_options);
    if (!ret.isOK()) {
        return ret;
    }
#endif

#ifdef MONGO_CONFIG_SSL
    moe::OptionSection ssl_options("SSL options");

    ret = addSSLServerOptions(&ssl_options);
    if (!ret.isOK()) {
        return ret;
    }
#endif

    moe::OptionSection sharding_options("Sharding options");

    sharding_options.addOptionChaining("sharding.configDB",
                                       "configdb",
                                       moe::String,
                                       "Connection string for communicating with config servers:\n"
                                       "<config replset name>/<host1:port>,<host2:port>,[...]");

    sharding_options.addOptionChaining(
        "replication.localPingThresholdMs",
        "localThreshold",
        moe::Int,
        "ping time (in ms) for a node to be considered local (default 15ms)");

    sharding_options.addOptionChaining("test", "test", moe::Switch, "just run unit tests")
        .setSources(moe::SourceAllLegacy);

    /** Javascript Options
     *  As a general rule, js enable/disable options are ignored for mongos.
     *  However, we define and hide these options so that if someone
     *  were to use these args in a set of options meant for both
     *  mongos and mongod runs, the mongos won't fail on an unknown argument.
     *
     *  These options have no affect on how the mongos runs.
     *  Setting either or both to *any* value will provoke a warning message
     *  and nothing more.
     */
    sharding_options
        .addOptionChaining("noscripting", "noscripting", moe::Switch, "disable scripting engine")
        .hidden()
        .setSources(moe::SourceAllLegacy);

    general_options
        .addOptionChaining(
            "security.javascriptEnabled", "", moe::Bool, "Enable javascript execution")
        .hidden()
        .setSources(moe::SourceYAMLConfig);

    options->addSection(general_options).transitional_ignore();

#if defined(_WIN32)
    options->addSection(windows_scm_options).transitional_ignore();
#endif

    options->addSection(sharding_options).transitional_ignore();

#ifdef MONGO_CONFIG_SSL
    options->addSection(ssl_options).transitional_ignore();
#endif

    return Status::OK();
}

void printMongosHelp(const moe::OptionSection& options) {
    std::cout << options.helpString() << std::endl;
};

bool handlePreValidationMongosOptions(const moe::Environment& params,
                                      const std::vector<std::string>& args) {
    if (params.count("help") && params["help"].as<bool>() == true) {
        printMongosHelp(moe::startupOptions);
        return false;
    }
    if (params.count("version") && params["version"].as<bool>() == true) {
        printShardingVersionInfo(true);
        return false;
    }
    if (params.count("test") && params["test"].as<bool>() == true) {
        ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
            ::mongo::logger::LogSeverity::Debug(5));
        StartupTest::runTests();
        return false;
    }

    return true;
}

Status validateMongosOptions(const moe::Environment& params) {
    Status ret = validateServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

Status canonicalizeMongosOptions(moe::Environment* params) {
    Status ret = canonicalizeServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

#ifdef MONGO_CONFIG_SSL
    ret = canonicalizeSSLServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }
#endif

    return Status::OK();
}

Status storeMongosOptions(const moe::Environment& params) {
    Status ret = storeServerOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    if (params.count("net.port")) {
        int port = params["net.port"].as<int>();
        if (port <= 0 || port > 65535) {
            return Status(ErrorCodes::BadValue, "error: port number must be between 1 and 65535");
        }
    }

    if (params.count("replication.localPingThresholdMs")) {
        serverGlobalParams.defaultLocalThresholdMillis =
            params["replication.localPingThresholdMs"].as<int>();
    }

    if (params.count("noscripting") || params.count("security.javascriptEnabled")) {
        warning() << "The Javascript enabled/disabled options are not supported for mongos. "
                     "(\"noscripting\" and/or \"security.javascriptEnabled\" are set.)";
    }

    if (!params.count("sharding.configDB")) {
        return Status(ErrorCodes::BadValue, "error: no args for --configdb");
    }

    std::string configdbString = params["sharding.configDB"].as<std::string>();

    auto configdbConnectionString = ConnectionString::parse(configdbString);
    if (!configdbConnectionString.isOK()) {
        return configdbConnectionString.getStatus();
    }

    if (configdbConnectionString.getValue().type() != ConnectionString::SET) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "configdb supports only replica set connection string");
    }

    std::vector<HostAndPort> seedServers;
    bool resolvedSomeSeedSever = false;
    for (const auto& host : configdbConnectionString.getValue().getServers()) {
        seedServers.push_back(host);
        if (!seedServers.back().hasPort()) {
            seedServers.back() = HostAndPort{host.host(), ServerGlobalParams::ConfigServerPort};
        }
        if (!hostbyname(seedServers.back().host().c_str()).empty()) {
            resolvedSomeSeedSever = true;
        }
    }
    if (!resolvedSomeSeedSever) {
        if (!hostbyname(configdbConnectionString.getValue().getSetName().c_str()).empty()) {
            warning() << "The replica set name \""
                      << escape(configdbConnectionString.getValue().getSetName())
                      << "\" resolves as a host name, but none of the servers in the seed list do. "
                         "Did you reverse the replica set name and the seed list in "
                      << escape(configdbConnectionString.getValue().toString()) << "?";
        }
    }

    mongosGlobalParams.configdbs =
        ConnectionString{configdbConnectionString.getValue().type(),
                         seedServers,
                         configdbConnectionString.getValue().getSetName()};

    if (mongosGlobalParams.configdbs.getServers().size() < 3) {
        warning() << "Running a sharded cluster with fewer than 3 config servers should only be "
                     "done for testing purposes and is not recommended for production.";
    }

    return Status::OK();
}

}  // namespace mongo
