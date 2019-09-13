
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/embedded/embedded_options.h"

#include "mongo/db/server_options.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/db/storage/mobile/mobile_global_options.h"
#include "mongo/db/storage/storage_options.h"

#include <boost/filesystem.hpp>
#include <string>

namespace mongo {
namespace embedded {

using std::string;

Status addOptions(optionenvironment::OptionSection* options) {
    moe::OptionSection general_options("General options");

    Status ret = addBaseServerOptions(&general_options);
    if (!ret.isOK()) {
        return ret;
    }

    moe::OptionSection storage_options("Storage options");

    storage_options
        .addOptionChaining(
            "storage.engine", "storageEngine", moe::String, "what storage engine to use")
        .setDefault(optionenvironment::Value("mobile"));

#ifdef _WIN32
    boost::filesystem::path currentPath = boost::filesystem::current_path();

    std::string defaultPath = currentPath.root_name().string() + storageGlobalParams.kDefaultDbPath;
    storage_options.addOptionChaining("storage.dbPath",
                                      "dbpath",
                                      optionenvironment::String,
                                      std::string("directory for datafiles - defaults to ") +
                                          storageGlobalParams.kDefaultDbPath + " which is " +
                                          defaultPath + " based on the current working drive");

#else
    storage_options.addOptionChaining("storage.dbPath",
                                      "dbpath",
                                      optionenvironment::String,
                                      std::string("directory for datafiles - defaults to ") +
                                          storageGlobalParams.kDefaultDbPath);

#endif

    storage_options.addOptionChaining("storage.repairPath",
                                      "repairpath",
                                      optionenvironment::String,
                                      "root directory for repair files - defaults to dbpath");

    options->addSection(general_options).transitional_ignore();
    options->addSection(storage_options).transitional_ignore();

    return Status::OK();
}

Status canonicalizeOptions(optionenvironment::Environment* params) {

    Status ret = canonicalizeBaseOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

Status storeOptions(const moe::Environment& params) {
    Status ret = storeBaseOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    if (params.count("storage.engine")) {
        storageGlobalParams.engine = params["storage.engine"].as<std::string>();
        storageGlobalParams.engineSetByUser = true;
    }

    if (params.count("storage.dbPath")) {
        storageGlobalParams.dbpath = params["storage.dbPath"].as<string>();
    }

    if (params.count("storage.mobile.durabilityLevel")) {
        mobileGlobalOptions.mobileDurabilityLevel =
            params["storage.mobile.durabilityLevel"].as<int>();
    }

#ifdef _WIN32
    if (storageGlobalParams.dbpath.size() > 1 &&
        storageGlobalParams.dbpath[storageGlobalParams.dbpath.size() - 1] == '/') {
        // size() check is for the unlikely possibility of --dbpath "/"
        storageGlobalParams.dbpath =
            storageGlobalParams.dbpath.erase(storageGlobalParams.dbpath.size() - 1);
    }
#endif

#ifdef _WIN32
    // If dbPath is a default value, prepend with drive name so log entries are explicit
    // We must resolve the dbpath before it stored in repairPath in the default case.
    if (storageGlobalParams.dbpath == storageGlobalParams.kDefaultDbPath ||
        storageGlobalParams.dbpath == storageGlobalParams.kDefaultConfigDbPath) {
        boost::filesystem::path currentPath = boost::filesystem::current_path();
        storageGlobalParams.dbpath = currentPath.root_name().string() + storageGlobalParams.dbpath;
    }
#endif

    // needs to be after things like --configsvr parsing, thus here.
    if (params.count("storage.repairPath")) {
        storageGlobalParams.repairpath = params["storage.repairPath"].as<string>();
        if (!storageGlobalParams.repairpath.size()) {
            return Status(ErrorCodes::BadValue, "repairpath is empty");
        }

        if (storageGlobalParams.dur &&
            !str::startsWith(storageGlobalParams.repairpath, storageGlobalParams.dbpath)) {
            return Status(ErrorCodes::BadValue,
                          "You must use a --repairpath that is a subdirectory of --dbpath when "
                          "using journaling");
        }
    } else {
        storageGlobalParams.repairpath = storageGlobalParams.dbpath;
    }

    return Status::OK();
}

void resetOptions() {
    storageGlobalParams.reset();
}

}  // namespace embedded
}  // namespace mongo
