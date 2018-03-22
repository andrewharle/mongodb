/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <iostream>
#include <vector>

#include "mongo/unittest/unittest.h"
#include "mongo/util/processinfo.h"

using mongo::ProcessInfo;
using boost::optional;

namespace mongo_test {
TEST(ProcessInfo, SysInfoIsInitialized) {
    ProcessInfo processInfo;
    if (processInfo.supported()) {
        ASSERT_FALSE(processInfo.getOsType().empty());
    }
}

TEST(ProcessInfo, NonZeroPageSize) {
    if (ProcessInfo::blockCheckSupported()) {
        ASSERT_GREATER_THAN(ProcessInfo::getPageSize(), 0u);
    }
}

TEST(ProcessInfo, GetNumAvailableCores) {
#if defined(__APPLE__) || defined(__linux__) || (defined(__sun) && defined(__SVR4)) || \
    defined(_WIN32)
    ProcessInfo processInfo;
    ProcessInfo::initializeSystemInfo();
    optional<unsigned long> numAvailCores = processInfo.getNumAvailableCores();
    ASSERT_TRUE(numAvailCores.is_initialized());
    ASSERT_GREATER_THAN(*numAvailCores, 0u);
    ASSERT_LESS_THAN_OR_EQUALS(*numAvailCores, processInfo.getNumCores());
#endif
}

TEST(ProcessInfo, GetNumCoresReturnsNonZeroNumberOfProcessors) {
    ProcessInfo processInfo;
    ProcessInfo::initializeSystemInfo();
    ASSERT_GREATER_THAN(processInfo.getNumCores(), 0u);
}
}
