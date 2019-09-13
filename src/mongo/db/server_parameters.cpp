// server_parameters.cpp


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

#include "mongo/base/parse_number.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::vector;

namespace {
ServerParameterSet* GLOBAL = NULL;
}

ServerParameter::ServerParameter(ServerParameterSet* sps,
                                 const std::string& name,
                                 bool allowedToChangeAtStartup,
                                 bool allowedToChangeAtRuntime)
    : _name(name),
      _allowedToChangeAtStartup(allowedToChangeAtStartup),
      _allowedToChangeAtRuntime(allowedToChangeAtRuntime) {
    if (sps) {
        sps->add(this);
    }
}

ServerParameter::ServerParameter(ServerParameterSet* sps, const std::string& name)
    : _name(name), _allowedToChangeAtStartup(true), _allowedToChangeAtRuntime(true) {
    if (sps) {
        sps->add(this);
    }
}

ServerParameter::~ServerParameter() {}

ServerParameterSet* ServerParameterSet::getGlobal() {
    if (!GLOBAL) {
        GLOBAL = new ServerParameterSet();
    }
    return GLOBAL;
}

void ServerParameterSet::add(ServerParameter* sp) {
    ServerParameter*& x = _map[sp->name()];
    if (x) {
        severe() << "'" << x->name() << "' already exists in the server parameter set.";
        abort();
    }
    x = sp;
}

}  // namespace mongo
