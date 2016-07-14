/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/string_map.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using Factory = Accumulator::Factory;

namespace {
// Used to keep track of which Accumulators are registered under which name.
static StringMap<Factory> factoryMap;
}  // namespace

void Accumulator::registerAccumulator(std::string name, Factory factory) {
    auto it = factoryMap.find(name);
    massert(28722,
            str::stream() << "Duplicate accumulator (" << name << ") registered.",
            it == factoryMap.end());
    factoryMap[name] = factory;
}

Factory Accumulator::getFactory(StringData name) {
    auto it = factoryMap.find(name);
    uassert(
        15952, str::stream() << "unknown group operator '" << name << "'", it != factoryMap.end());
    return it->second;
}

}  // namespace mongo
