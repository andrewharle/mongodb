// #file dbtests.cpp : Runs db unit tests.
//

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/base/initializer.h"
#include "mongo/db/commands.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/dbtests/framework.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/gcov.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/text.h"

namespace mongo {
    // This specifies default dbpath for our testing framework
    const std::string default_test_dbpath = "/tmp/unittest";
} // namespace mongo


int dbtestsMain( int argc, char** argv, char** envp ) {
    static StaticObserver StaticObserver;
    setWindowsUnhandledExceptionFilter();
    setGlobalAuthorizationManager(new AuthorizationManager(new AuthzManagerExternalStateMock()));
    Command::testCommandsEnabled = 1;
    mongo::runGlobalInitializersOrDie(argc, argv, envp);
    StartupTest::runTests();
    return mongo::dbtests::runDbTests(argc, argv);
}

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables dbtestsMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = dbtestsMain(argc, wcl.argv(), wcl.envp());
    ::_exit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = dbtestsMain(argc, argv, envp);
    flushForGcov();
    ::_exit(exitCode);
}
#endif
