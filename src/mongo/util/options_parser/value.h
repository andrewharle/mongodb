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

#include <map>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"

namespace mongo {
namespace optionenvironment {

    class Constraint;
    class KeyConstraint;

    /**
     * Helper typedefs for the more complex C++ types supported by this Value class
     */
    typedef std::map<std::string, std::string> StringMap_t;
    typedef std::vector<std::string> StringVector_t;

    typedef std::string Key;

    /** A simple container interface for storing various C++ values.
     *
     *  Usage:
     *
     *  Value intVal(2);
     *  Value stringVal("string");
     *
     *  int intContents = 1;
     *  Status ret = stringVal.get(&intContents);
     *  // ret != Status::OK()
     *  // intContents is still 1
     *
     *  ret = intVal.get(&intContents);
     *  // ret == Status::OK()
     *  // intContents is now 2
     */
    class Value {
    public:

        // Constructors

        explicit Value() : _type(None) { }
        explicit Value(StringVector_t val) : _stringVectorVal(val), _type(StringVector) {}
        explicit Value(StringMap_t val) : _stringMapVal(val), _type(StringMap) {}
        explicit Value(bool val) : _boolVal(val), _type(Bool) { }
        explicit Value(double val) : _doubleVal(val), _type(Double) { }
        explicit Value(int val) : _intVal(val), _type(Int) { }
        explicit Value(long val) : _longVal(val), _type(Long) { }
        explicit Value(std::string val) : _stringVal(val), _type(String) { }
        explicit Value(unsigned long long val) : _unsignedLongLongVal(val),
                                                 _type(UnsignedLongLong) { }
        explicit Value(unsigned val) : _unsignedVal(val), _type(Unsigned) { }

        // Access interface

        Status get(StringVector_t* val) const;
        Status get(StringMap_t* val) const;
        Status get(bool* val) const;
        Status get(double* val) const;
        Status get(int* val) const;
        Status get(long* val) const;
        Status get(string* val) const;
        Status get(unsigned long long* val) const;
        Status get(unsigned* val) const;

        // Utility functions

        /**
         *  Return the value's type as a string
         */
        std::string typeToString() const;

        /**
         *  Return true if the value was created with the no argument constructor
         */
        bool isEmpty() const;

        /**
         *  Return true if the other Value equals this value, both in type and in contents
         *
         *  Two empty values are equal
         */
        bool equal(const Value&) const;

        /**
         *  Return the string representation of this Value.  This function is used only for
         *  debugging purposes and does not output data in an easily parseable format.
         */
        std::string toString() const;

        /**
         *  The functions below are the legacy interface to be consistent with boost::any during the
         *  transition period
         */

        /**
         *  Returns the contents of this Value as type T.  Throws MsgAssertionException if the type
         *  does not match
         */
        template <typename T>
        T as() const;

        /**
         *  Return the type_info for this value
         */
        const std::type_info& type() const;

    private:
        StringVector_t _stringVectorVal;
        StringMap_t _stringMapVal;
        std::string _stringVal;
        union {
            bool _boolVal;
            double _doubleVal;
            int _intVal;
            long _longVal;
            unsigned long long _unsignedLongLongVal;
            unsigned _unsignedVal;
        };

        // Types currently supported by Value
        enum Type {
            StringVector,     // std::vector<std::string>
            StringMap,        // std::map<std::string, std::string>
            Bool,             // bool
            Double,           // double
            Int,              // int
            Long,             // long
            String,           // std::string
            UnsignedLongLong, // unsigned long long
            Unsigned,         // unsigned
            None,             // (not set)
        };

        Type _type;
    };

    template <typename T>
    T Value::as() const {
        T valueType;

        Status ret = get(&valueType);
        if (!ret.isOK()) {
            StringBuilder message;
            message << "failed to extract typed value from Value container: " << ret.toString();
            throw MsgAssertionException(17114, message.str());
        }

        return valueType;
    }

} // namespace optionenvironment
} // namespace mongo
