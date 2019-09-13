// processinfo.cpp


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

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/util/processinfo.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <fstream>
#include <iostream>

#include "mongo/util/log.h"

using namespace std;

namespace mongo {

class PidFileWiper {
public:
    ~PidFileWiper() {
        if (path.empty()) {
            return;
        }

        ofstream out(path.c_str(), ios_base::out);
        out.close();
    }

    bool write(const boost::filesystem::path& p) {
        path = p;
        ofstream out(path.c_str(), ios_base::out);
        out << ProcessId::getCurrent() << endl;
        if (!out.good()) {
            auto errAndStr = errnoAndDescription();
            if (errAndStr.first == 0) {
                log() << "ERROR: Cannot write pid file to " << path.string()
                      << ": Unable to determine OS error";
            } else {
                log() << "ERROR: Cannot write pid file to " << path.string() << ": "
                      << errAndStr.second;
            }
        } else {
            boost::system::error_code ec;
            boost::filesystem::permissions(
                path,
                boost::filesystem::owner_read | boost::filesystem::owner_write |
                    boost::filesystem::group_read | boost::filesystem::others_read,
                ec);
            if (ec) {
                log() << "Could not set permissions on pid file " << path.string() << ": "
                      << ec.message();
                return false;
            }
        }
        return out.good();
    }

private:
    boost::filesystem::path path;
} pidFileWiper;

bool writePidFile(const string& path) {
    return pidFileWiper.write(path);
}
}  // namespace mongo
