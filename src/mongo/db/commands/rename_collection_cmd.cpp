
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

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::min;
using std::string;
using std::stringstream;

namespace {

class CmdRenameCollection : public ErrmsgCommandDeprecated {
public:
    CmdRenameCollection() : ErrmsgCommandDeprecated("renameCollection") {}
    virtual bool adminOnly() const {
        return true;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return rename_collection::checkAuthForRenameCollectionCommand(client, dbname, cmdObj);
    }
    std::string help() const override {
        return " example: { renameCollection: foo.a, to: bar.b }";
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    static void dropCollection(OperationContext* opCtx, Database* db, StringData collName) {
        WriteUnitOfWork wunit(opCtx);
        if (db->dropCollection(opCtx, collName).isOK()) {
            // ignoring failure case
            wunit.commit();
        }
    }

    virtual bool errmsgRun(OperationContext* opCtx,
                           const string& dbname,
                           const BSONObj& cmdObj,
                           string& errmsg,
                           BSONObjBuilder& result) {
        const auto sourceNsElt = cmdObj[getName()];
        const auto targetNsElt = cmdObj["to"];

        uassert(ErrorCodes::TypeMismatch,
                "'renameCollection' must be of type String",
                sourceNsElt.type() == BSONType::String);
        uassert(ErrorCodes::TypeMismatch,
                "'to' must be of type String",
                targetNsElt.type() == BSONType::String);

        const NamespaceString source(sourceNsElt.valueStringData());
        const NamespaceString target(targetNsElt.valueStringData());

        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid source namespace: " << source.ns(),
                source.isValid());
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid target namespace: " << target.ns(),
                target.isValid());

        if ((repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
             repl::ReplicationCoordinator::modeNone)) {
            if (source.isOplog()) {
                errmsg = "can't rename live oplog while replicating";
                return false;
            }
            if (target.isOplog()) {
                errmsg = "can't rename to live oplog while replicating";
                return false;
            }
        }

        if (source.isOplog() != target.isOplog()) {
            errmsg = "If either the source or target of a rename is an oplog name, both must be";
            return false;
        }

        Status sourceStatus = userAllowedWriteNS(source);
        if (!sourceStatus.isOK()) {
            errmsg = "error with source namespace: " + sourceStatus.reason();
            return false;
        }

        Status targetStatus = userAllowedWriteNS(target);
        if (!targetStatus.isOK()) {
            errmsg = "error with target namespace: " + targetStatus.reason();
            return false;
        }

        if (source.isSystemDotIndexes() || target.isSystemDotIndexes()) {
            errmsg = "renaming system.indexes is not allowed";
            return false;
        }

        if (source.isServerConfigurationCollection()) {
            uasserted(ErrorCodes::IllegalOperation,
                      "renaming the server configuration "
                      "collection (admin.system.version) is not "
                      "allowed");
        }

        RenameCollectionOptions options;
        options.dropTarget = cmdObj["dropTarget"].trueValue();
        options.stayTemp = cmdObj["stayTemp"].trueValue();
        uassertStatusOK(renameCollection(opCtx, source, target, options));
        return true;
    }

} cmdrenamecollection;

}  // namespace
}  // namespace mongo
