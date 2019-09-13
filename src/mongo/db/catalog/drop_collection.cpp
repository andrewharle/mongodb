
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

#include "mongo/db/catalog/drop_collection.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/log.h"

namespace mongo {

Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& collectionName,
                      BSONObjBuilder& result,
                      const repl::OpTime& dropOpTime,
                      DropCollectionSystemCollectionMode systemCollectionMode) {
    if (!serverGlobalParams.quiet.load()) {
        log() << "CMD: drop " << collectionName;
    }

    const std::string dbname = collectionName.db().toString();

    return writeConflictRetry(opCtx, "drop", collectionName.ns(), [&] {
        AutoGetDb autoDb(opCtx, dbname, MODE_X);
        Database* const db = autoDb.getDb();
        Collection* coll = db ? db->getCollection(opCtx, collectionName) : nullptr;
        auto view =
            db && !coll ? db->getViewCatalog()->lookup(opCtx, collectionName.ns()) : nullptr;

        if (!db || (!coll && !view)) {
            return Status(ErrorCodes::NamespaceNotFound, "ns not found");
        }

        const bool shardVersionCheck = true;
        OldClientContext context(opCtx, collectionName.ns(), shardVersionCheck);

        bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, collectionName);

        if (userInitiatedWritesAndNotPrimary) {
            return Status(ErrorCodes::NotMaster,
                          str::stream() << "Not primary while dropping collection "
                                        << collectionName.ns());
        }

        WriteUnitOfWork wunit(opCtx);
        if (!result.hasField("ns")) {
            result.append("ns", collectionName.ns());
        }

        if (coll) {
            invariant(!view);
            int numIndexes = coll->getIndexCatalog()->numIndexesTotal(opCtx);

            BackgroundOperation::assertNoBgOpInProgForNs(collectionName.ns());

            Status s = systemCollectionMode ==
                    DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops
                ? db->dropCollection(opCtx, collectionName.ns(), dropOpTime)
                : db->dropCollectionEvenIfSystem(opCtx, collectionName, dropOpTime);

            if (!s.isOK()) {
                return s;
            }

            result.append("nIndexesWas", numIndexes);
        } else {
            invariant(view);
            Status status = db->dropView(opCtx, collectionName.ns());
            if (!status.isOK()) {
                return status;
            }
        }
        wunit.commit();

        return Status::OK();
    });
}

}  // namespace mongo
