
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand
#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/cloner.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace {

using namespace mongo;

using std::set;
using std::string;
using std::stringstream;

/* The clone command is deprecated. See http://dochub.mongodb.org/core/copydb-clone-deprecation.
   Usage:
   mydb.$cmd.findOne( { clone: "fromhost" } );
   Note: doesn't work with authentication enabled, except as internal operation or for
   old-style users for backwards compatibility.
*/
class CmdClone : public BasicCommand {
public:
    CmdClone() : BasicCommand("clone") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "clone this database from an instance of the db on another host\n"
               "{clone: \"host13\"[, slaveOk: <bool>]}";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        ActionSet actions;
        actions.addAction(ActionType::insert);
        actions.addAction(ActionType::createIndex);
        if (shouldBypassDocumentValidationForCommand(cmdObj)) {
            actions.addAction(ActionType::bypassDocumentValidation);
        }

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(dbname), actions)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        const char* deprecationWarning =
            "Support for the clone command has been deprecated. See "
            "http://dochub.mongodb.org/core/copydb-clone-deprecation";
        warning() << deprecationWarning;
        result.append("note", deprecationWarning);
        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj)) {
            maybeDisableValidation.emplace(opCtx);
        }

        string from = cmdObj.getStringField("clone");
        if (from.empty())
            return false;

        CloneOptions opts;
        opts.fromDB = dbname;
        opts.slaveOk = cmdObj["slaveOk"].trueValue();

        // collsToIgnore is only used by movePrimary and contains a list of the
        // sharded collections.
        if (cmdObj["collsToIgnore"].type() == Array) {
            BSONObjIterator it(cmdObj["collsToIgnore"].Obj());

            while (it.more()) {
                BSONElement e = it.next();
                if (e.type() == String) {
                    opts.shardedColls.insert(e.String());
                }
            }
        }

        // Clone the non-ignored collections.
        set<string> clonedColls;
        Lock::DBLock dbXLock(opCtx, dbname, MODE_X);

        Cloner cloner;
        Status status = cloner.copyDb(opCtx, dbname, from, opts, &clonedColls);

        BSONArrayBuilder barr;
        barr.append(clonedColls);
        result.append("clonedColls", barr.arr());

        uassertStatusOK(status);
        return true;
    }

} cmdClone;

}  // namespace
