// mongo/unittest/unittest_main.cpp


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

#include <iostream>
#include <string>
#include <vector>

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/signal_handlers_synchronous.h"

using mongo::Status;

int main(int argc, char** argv, char** envp) {
    ::mongo::clearSignalMask();
    ::mongo::setupSynchronousSignalHandlers();

    ::mongo::runGlobalInitializersOrDie(argc, argv, envp);

    namespace moe = ::mongo::optionenvironment;
    moe::OptionsParser parser;
    moe::Environment environment;
    moe::OptionSection options;
    std::map<std::string, std::string> env;

    // Register our allowed options with our OptionSection
    auto listDesc = "List all test suites in this unit test.";
    options.addOptionChaining("list", "list", moe::Switch, listDesc).setDefault(moe::Value(false));

    auto suiteDesc = "Test suite name. Specify --suite more than once to run multiple suites.";
    options.addOptionChaining("suite", "suite", moe::StringVector, suiteDesc);

    auto filterDesc = "Test case name filter. Specify the substring of the test names.";
    options.addOptionChaining("filter", "filter", moe::String, filterDesc);

    auto repeatDesc = "Specifies the number of runs for each test.";
    options.addOptionChaining("repeat", "repeat", moe::Int, repeatDesc).setDefault(moe::Value(1));

    std::vector<std::string> argVector(argv, argv + argc);
    Status ret = parser.run(options, argVector, env, &environment);
    if (!ret.isOK()) {
        std::cerr << options.helpString();
        return EXIT_FAILURE;
    }

    bool list = false;
    moe::StringVector_t suites;
    std::string filter;
    int repeat = 1;
    // "list" and "repeat" will be assigned with default values, if not present.
    invariant(environment.get("list", &list));
    invariant(environment.get("repeat", &repeat));
    // The default values of "suite" and "filter" are empty.
    environment.get("suite", &suites).ignore();
    environment.get("filter", &filter).ignore();

    if (list) {
        auto suiteNames = ::mongo::unittest::getAllSuiteNames();
        for (auto name : suiteNames) {
            std::cout << name << std::endl;
        }
        return EXIT_SUCCESS;
    }
    return ::mongo::unittest::Suite::run(suites, filter, repeat);
}
