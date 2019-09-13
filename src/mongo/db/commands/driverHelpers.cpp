// driverHelpers.cpp


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

/**
   this file has dbcommands that are for drivers
   mostly helpers
*/


#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/background.h"

namespace mongo {

using std::string;

class BasicDriverHelper : public ErrmsgCommandDeprecated {
public:
    BasicDriverHelper(const char* name) : ErrmsgCommandDeprecated(name) {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
};

class ObjectIdTest : public BasicDriverHelper {
public:
    ObjectIdTest() : BasicDriverHelper("driverOIDTest") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}  // No auth required
    virtual bool errmsgRun(OperationContext* opCtx,
                           const string&,
                           const BSONObj& cmdObj,
                           string& errmsg,
                           BSONObjBuilder& result) {
        if (cmdObj.firstElement().type() != jstOID) {
            errmsg = "not oid";
            return false;
        }

        const OID& oid = cmdObj.firstElement().__oid();
        result.append("oid", oid);
        result.append("str", oid.toString());

        return true;
    }
} driverObjectIdTest;
}
