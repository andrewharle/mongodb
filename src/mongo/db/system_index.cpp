
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/system_index.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/client/index_spec.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_rebuilder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

using namespace std::chrono_literals;

namespace mongo {

namespace {
BSONObj v1SystemUsersKeyPattern;
BSONObj v3SystemUsersKeyPattern;
BSONObj v3SystemRolesKeyPattern;
std::string v3SystemUsersIndexName;
std::string v3SystemRolesIndexName;
IndexSpec v3SystemUsersIndexSpec;
IndexSpec v3SystemRolesIndexSpec;

const NamespaceString sessionCollectionNamespace("config.system.sessions");

MONGO_INITIALIZER(AuthIndexKeyPatterns)(InitializerContext*) {
    v1SystemUsersKeyPattern = BSON("user" << 1 << "userSource" << 1);
    v3SystemUsersKeyPattern = BSON(
        AuthorizationManager::USER_NAME_FIELD_NAME << 1 << AuthorizationManager::USER_DB_FIELD_NAME
                                                   << 1);
    v3SystemRolesKeyPattern = BSON(
        AuthorizationManager::ROLE_NAME_FIELD_NAME << 1 << AuthorizationManager::ROLE_DB_FIELD_NAME
                                                   << 1);
    v3SystemUsersIndexName =
        std::string(str::stream() << AuthorizationManager::USER_NAME_FIELD_NAME << "_1_"
                                  << AuthorizationManager::USER_DB_FIELD_NAME
                                  << "_1");
    v3SystemRolesIndexName =
        std::string(str::stream() << AuthorizationManager::ROLE_NAME_FIELD_NAME << "_1_"
                                  << AuthorizationManager::ROLE_DB_FIELD_NAME
                                  << "_1");

    v3SystemUsersIndexSpec.addKeys(v3SystemUsersKeyPattern);
    v3SystemUsersIndexSpec.unique();
    v3SystemUsersIndexSpec.name(v3SystemUsersIndexName);

    v3SystemRolesIndexSpec.addKeys(v3SystemRolesKeyPattern);
    v3SystemRolesIndexSpec.unique();
    v3SystemRolesIndexSpec.name(v3SystemRolesIndexName);

    return Status::OK();
}

void generateSystemIndexForExistingCollection(OperationContext* opCtx,
                                              Collection* collection,
                                              const NamespaceString& ns,
                                              const IndexSpec& spec) {
    // Do not try and generate any system indexes in read only mode.
    if (storageGlobalParams.readOnly) {
        warning() << "Running in queryable backup mode. Unable to create authorization index on "
                  << ns;
        return;
    }

    // Do not try to generate any system indexes on a secondary.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    uassert(ErrorCodes::NotMaster,
            "Not primary while creating authorization index",
            replCoord->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet ||
                replCoord->canAcceptWritesForDatabase(opCtx, ns.db()));

    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    try {
        auto indexSpecStatus = index_key_validate::validateIndexSpec(
            opCtx, spec.toBSON(), ns, serverGlobalParams.featureCompatibility);
        BSONObj indexSpec = fassert(40452, indexSpecStatus);

        log() << "No authorization index detected on " << ns
              << " collection. Attempting to recover by creating an index with spec: " << indexSpec;

        MultiIndexBlock indexer(opCtx, collection);

        writeConflictRetry(opCtx, "authorization index regeneration", ns.ns(), [&] {
            fassert(40453, indexer.init(indexSpec));
        });

        fassert(40454, indexer.insertAllDocumentsInCollection());

        writeConflictRetry(opCtx, "authorization index regeneration", ns.ns(), [&] {
            WriteUnitOfWork wunit(opCtx);

            indexer.commit([opCtx, &ns, collection](const BSONObj& spec) {
                opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                    opCtx, ns, collection->uuid(), spec, false /* fromMigrate */);
            });

            wunit.commit();
        });

        log() << "Authorization index construction on " << ns << " is complete";
    } catch (const DBException& e) {
        severe() << "Failed to regenerate index for " << ns << ". Exception: " << e.what();
        throw;
    }
}

Status createOrRebuildIndex(OperationContext* opCtx,
                            Collection* collection,
                            const BSONObj& indexPattern,
                            const IndexSpec& indexSpec) {
    IndexCatalog* indexCatalog = collection->getIndexCatalog();
    invariant(indexCatalog);

    if (!indexCatalog->checkUnfinished().isOK()) {
        try {
            forceRestartInProgressIndexesOnCollection(opCtx, collection->ns());
        } catch (...) {
            return exceptionToStatus();
        }
    }

    std::vector<IndexDescriptor*> indexes;
    indexCatalog->findIndexesByKeyPattern(opCtx, indexPattern, false, &indexes);

    if (indexes.empty()) {
        try {
            generateSystemIndexForExistingCollection(
                opCtx, collection, collection->ns(), indexSpec);
        } catch (...) {
            return exceptionToStatus();
        }
    }

    return Status::OK();
}

}  // namespace

Status verifySystemIndexes(OperationContext* opCtx) {
    const NamespaceString& systemUsers = AuthorizationManager::usersCollectionNamespace;
    const NamespaceString& systemRoles = AuthorizationManager::rolesCollectionNamespace;

    // Create indexes for collections on the admin db
    {
        AutoGetDb autoDb(opCtx, systemUsers.db(), MODE_X);
        if (!autoDb.getDb()) {
            return Status::OK();
        }

        Collection* collection = autoDb.getDb()->getCollection(opCtx, systemUsers);
        if (collection) {
            IndexCatalog* indexCatalog = collection->getIndexCatalog();
            invariant(indexCatalog);

            // Make sure the old unique index from v2.4 on system.users doesn't exist.
            std::vector<IndexDescriptor*> indexes;
            indexCatalog->findIndexesByKeyPattern(opCtx, v1SystemUsersKeyPattern, false, &indexes);

            if (!indexes.empty()) {
                fassert(ErrorCodes::AmbiguousIndexKeyPattern, indexes.size() == 1);
                return Status(ErrorCodes::AuthSchemaIncompatible,
                              "Old 2.4 style user index identified. "
                              "The authentication schema needs to be updated by "
                              "running authSchemaUpgrade on a 2.6 server.");
            }

            // Ensure that system indexes exist for the user collection
            Status createRebuildStatus = createOrRebuildIndex(
                opCtx, collection, v3SystemUsersKeyPattern, v3SystemUsersIndexSpec);
            if (!createRebuildStatus.isOK()) {
                return createRebuildStatus;
            }
        }

        // Ensure that system indexes exist for the roles collection, if it exists.
        collection = autoDb.getDb()->getCollection(opCtx, systemRoles);
        if (collection) {
            Status createRebuildStatus = createOrRebuildIndex(
                opCtx, collection, v3SystemRolesKeyPattern, v3SystemRolesIndexSpec);
            if (!createRebuildStatus.isOK()) {
                return createRebuildStatus;
            }
        }
    }

    return Status::OK();
}

void createSystemIndexes(OperationContext* opCtx, Collection* collection) {
    invariant(collection);
    const NamespaceString& ns = collection->ns();
    BSONObj indexSpec;
    if (ns == AuthorizationManager::usersCollectionNamespace) {
        indexSpec =
            fassert(40455,
                    index_key_validate::validateIndexSpec(opCtx,
                                                          v3SystemUsersIndexSpec.toBSON(),
                                                          ns,
                                                          serverGlobalParams.featureCompatibility));

    } else if (ns == AuthorizationManager::rolesCollectionNamespace) {
        indexSpec =
            fassert(40457,
                    index_key_validate::validateIndexSpec(opCtx,
                                                          v3SystemRolesIndexSpec.toBSON(),
                                                          ns,
                                                          serverGlobalParams.featureCompatibility));
    }
    if (!indexSpec.isEmpty()) {
        opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
            opCtx, ns, collection->uuid(), indexSpec, false /* fromMigrate */);
        // Note that the opObserver is called prior to creating the index.  This ensures the index
        // write gets the same storage timestamp as the oplog entry.
        fassert(40456,
                collection->getIndexCatalog()->createIndexOnEmptyCollection(opCtx, indexSpec));
    }
}

}  // namespace mongo
