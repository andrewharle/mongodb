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

#pragma once
#include "mongo/util/options_parser/option_description.h"

#include <boost/program_options.hpp>
#include <boost/shared_ptr.hpp>
#include <iostream>
#include <list>

#include "mongo/base/status.h"

namespace mongo {
namespace optionenvironment {

    namespace po = boost::program_options;

    /**
     *  A container for OptionDescription instances as well as other OptionSection instances.
     *  Provides a description of all options that are supported to be passed in to an
     *  OptionsParser.  Has utility functions to support the various formats needed by the parsing
     *  process
     *
     *  The sections and section names only matter in the help string.  For sections in a JSON
     *  config, look at the dots in the dottedName of the relevant OptionDescription
     *
     *  Usage:
     *
     *  namespace moe = mongo::optionenvironment;
     *
     *  moe::OptionsParser parser;
     *  moe::Environment environment;
     *  moe::OptionSection options;
     *  moe::OptionSection subSection("Section Name");
     *
     *  // Register our allowed option flags with our OptionSection
     *  options.addOptionChaining("help", "help", moe::Switch, "Display Help");
     *
     *  // Register our positional options with our OptionSection
     *  options.addOptionChaining("command", "command", moe::String, "Command").positional(1, 1);
     *
     *  // Add a subsection
     *  subSection.addOptionChaining("port", "port", moe::Int, "Port");
     *  options.addSection(subSection);
     *
     *  // Run the parser
     *  Status ret = parser.run(options, argc, argv, envp, &environment);
     *  if (!ret.isOK()) {
     *      cerr << options.helpString() << endl;
     *      exit(EXIT_FAILURE);
     *  }
     */

    class OptionSection {
    public:
        OptionSection(const std::string& name) : _name(name) { }
        OptionSection() { }

        // Construction interface

        /**
         * Add a sub section to this section.  Used mainly to keep track of section headers for when
         * we need generate the help string for the command line
         */
        Status addSection(const OptionSection& subSection);

        /**
         * Add an option to this section, and returns a reference to an OptionDescription to allow
         * for chaining.
         *
         * Examples:
         *
         * options.addOptionChaining("option", "option", moe::String, "Chaining Registration")
         *                          .hidden().setDefault(moe::Value("default"))
         *                          .setImplicit(moe::Value("implicit"));
         *
         * This creates a hidden option that has default and implicit values.
         *
         * options.addOptionChaining("name", "name", moe::String, "Composing Option")
         *                          .composing().sources(SourceAllConfig);
         *
         * This creates an option that is composing and can be specified only in config files.
         *
         * See the OptionDescription class for details on the supported attributes.
         *
         * throws DBException on errors, such as attempting to register an option with the same name
         * as another option.  These represent programming errors that should not happen during
         * normal operation.
         */
        OptionDescription& addOptionChaining(const std::string& dottedName,
                                             const std::string& singleName,
                                             const OptionType type,
                                             const std::string& description);

        // These functions are used by the OptionsParser to make calls into boost::program_options
        Status getBoostOptions(po::options_description* boostOptions,
                               bool visibleOnly = false,
                               bool includeDefaults = false,
                               OptionSources = SourceAll,
                               bool getEmptySections = true) const;
        Status getBoostPositionalOptions(
                po::positional_options_description* boostPositionalOptions) const;

        // This is needed so that the parser can iterate over all registered options to get the
        // correct names when populating the Environment, as well as check that a parameter that was
        // found has been registered and has the correct type
        Status getAllOptions(std::vector<OptionDescription>* options) const;

        // Count the number of options in this section and all subsections
        Status countOptions(int* numOptions, bool visibleOnly, OptionSources sources) const;

        /**
         * Populates the given map with all the default values for any options in this option
         * section and all sub sections.
         */
        Status getDefaults(std::map<Key, Value>* values) const;

        /**
         * Populates the given vector with all the constraints for all options in this section and
         * sub sections.
         */
        Status getConstraints(std::vector<boost::shared_ptr<Constraint > >* constraints) const;

        std::string positionalHelpString(const std::string& execName) const;
        std::string helpString() const;

        // Debugging
        void dump() const;

    private:
        std::string _name;
        std::list<OptionSection> _subSections;
        std::list<OptionDescription> _options;
    };

} // namespace optionenvironment
} // namespace mongo
