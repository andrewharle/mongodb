// collection_to_capped.cpp


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

#include "mongo/platform/basic.h"


#include "mongo/db/background.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using std::stringstream;

class CmdCloneCollectionAsCapped : public ErrmsgCommandDeprecated {
public:
    CmdCloneCollectionAsCapped() : ErrmsgCommandDeprecated("cloneCollectionAsCapped") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    std::string help() const override {
        return "{ cloneCollectionAsCapped:<fromName>, toCollection:<toName>, size:<sizeInBytes> }";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet sourceActions;
        sourceActions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), sourceActions));

        ActionSet targetActions;
        targetActions.addAction(ActionType::insert);
        targetActions.addAction(ActionType::createIndex);
        targetActions.addAction(ActionType::convertToCapped);

        const auto nssElt = cmdObj["toCollection"];
        uassert(ErrorCodes::TypeMismatch,
                "'toCollection' must be of type String",
                nssElt.type() == BSONType::String);
        const NamespaceString nss(dbname, nssElt.valueStringData());
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid target namespace: " << nss.ns(),
                nss.isValid());

        out->push_back(Privilege(ResourcePattern::forExactNamespace(nss), targetActions));
    }
    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& jsobj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        const auto fromElt = jsobj["cloneCollectionAsCapped"];
        const auto toElt = jsobj["toCollection"];

        uassert(ErrorCodes::TypeMismatch,
                "'cloneCollectionAsCapped' must be of type String",
                fromElt.type() == BSONType::String);
        uassert(ErrorCodes::TypeMismatch,
                "'toCollection' must be of type String",
                toElt.type() == BSONType::String);

        const StringData from(fromElt.valueStringData());
        const StringData to(toElt.valueStringData());

        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid source collection name: " << from,
                NamespaceString::validCollectionName(from));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid target collection name: " << to,
                NamespaceString::validCollectionName(to));

        double size = jsobj.getField("size").number();
        bool temp = jsobj.getField("temp").trueValue();

        if (size == 0) {
            errmsg = "invalid command spec";
            return false;
        }

        AutoGetDb autoDb(opCtx, dbname, MODE_X);

        NamespaceString nss(dbname, to);
        if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
            uasserted(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while cloning collection " << from << " to "
                                    << to
                                    << " (as capped)");
        }

        Database* const db = autoDb.getDb();
        if (!db) {
            uasserted(ErrorCodes::NamespaceNotFound,
                      str::stream() << "database " << dbname << " not found");
        }

        Status status =
            cloneCollectionAsCapped(opCtx, db, from.toString(), to.toString(), size, temp);
        uassertStatusOK(status);
        return true;
    }
} cmdCloneCollectionAsCapped;

/* jan2010:
   Converts the given collection to a capped collection w/ the specified size.
   This command is not highly used, and is not currently supported with sharded
   environments.
   */
class CmdConvertToCapped : public ErrmsgCommandDeprecated {
public:
    CmdConvertToCapped() : ErrmsgCommandDeprecated("convertToCapped") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    std::string help() const override {
        return "{ convertToCapped:<fromCollectionName>, size:<sizeInBytes> }";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::convertToCapped);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& jsobj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, jsobj));
        long long size = jsobj.getField("size").safeNumberLong();

        if (size == 0) {
            errmsg = "invalid command spec";
            return false;
        }

        uassertStatusOK(convertToCapped(opCtx, nss, size));
        return true;
    }

} cmdConvertToCapped;
}
