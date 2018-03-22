/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands/feature_compatibility_version.h"

#include "mongo/base/status.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/commands/feature_compatibility_version_command_parser.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"

namespace mongo {

using repl::UnreplicatedWritesBlock;

constexpr StringData FeatureCompatibilityVersion::k32IncompatibleIndexName;
constexpr StringData FeatureCompatibilityVersion::kCollection;
constexpr StringData FeatureCompatibilityVersion::kCommandName;
constexpr StringData FeatureCompatibilityVersion::kDatabase;
constexpr StringData FeatureCompatibilityVersion::kParameterName;
constexpr StringData FeatureCompatibilityVersion::kVersionField;

namespace {
const BSONObj k32IncompatibleIndexSpec =
    BSON(IndexDescriptor::kIndexVersionFieldName
         << static_cast<int>(IndexDescriptor::IndexVersion::kV2)
         << IndexDescriptor::kKeyPatternFieldName
         << BSON(FeatureCompatibilityVersion::kVersionField << 1)
         << IndexDescriptor::kNamespaceFieldName
         << FeatureCompatibilityVersion::kCollection
         << IndexDescriptor::kIndexNameFieldName
         << FeatureCompatibilityVersion::k32IncompatibleIndexName);

BSONObj makeUpdateCommand(StringData newVersion, BSONObj writeConcern) {
    BSONObjBuilder updateCmd;

    NamespaceString nss(FeatureCompatibilityVersion::kCollection);
    updateCmd.append("update", nss.coll());
    {
        BSONArrayBuilder updates(updateCmd.subarrayStart("updates"));
        {
            BSONObjBuilder updateSpec(updates.subobjStart());
            {
                BSONObjBuilder queryFilter(updateSpec.subobjStart("q"));
                queryFilter.append("_id", FeatureCompatibilityVersion::kParameterName);
            }
            {
                BSONObjBuilder updateMods(updateSpec.subobjStart("u"));
                updateMods.append(FeatureCompatibilityVersion::kVersionField, newVersion);
            }
            updateSpec.appendBool("upsert", true);
        }
    }

    if (!writeConcern.isEmpty()) {
        updateCmd.append(WriteConcernOptions::kWriteConcernField, writeConcern);
    }

    return updateCmd.obj();
}

StringData getFeatureCompatibilityVersionString(
    ServerGlobalParams::FeatureCompatibility::Version version) {
    switch (version) {
        case ServerGlobalParams::FeatureCompatibility::Version::k34:
            return FeatureCompatibilityVersionCommandParser::kVersion34;
        case ServerGlobalParams::FeatureCompatibility::Version::k32:
            return FeatureCompatibilityVersionCommandParser::kVersion32;
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace

StatusWith<ServerGlobalParams::FeatureCompatibility::Version> FeatureCompatibilityVersion::parse(
    const BSONObj& featureCompatibilityVersionDoc) {
    bool foundVersionField = false;
    ServerGlobalParams::FeatureCompatibility::Version version;

    for (auto&& elem : featureCompatibilityVersionDoc) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == "_id") {
            continue;
        } else if (fieldName == FeatureCompatibilityVersion::kVersionField) {
            foundVersionField = true;
            if (elem.type() != BSONType::String) {
                return Status(
                    ErrorCodes::TypeMismatch,
                    str::stream()
                        << FeatureCompatibilityVersion::kVersionField
                        << " must be of type String, but was of type "
                        << typeName(elem.type())
                        << ". Contents of "
                        << FeatureCompatibilityVersion::kParameterName
                        << " document in "
                        << FeatureCompatibilityVersion::kCollection
                        << ": "
                        << featureCompatibilityVersionDoc
                        << ". See http://dochub.mongodb.org/core/3.4-feature-compatibility.");
            }
            if (elem.String() == FeatureCompatibilityVersionCommandParser::kVersion34) {
                version = ServerGlobalParams::FeatureCompatibility::Version::k34;
            } else if (elem.String() == FeatureCompatibilityVersionCommandParser::kVersion32) {
                version = ServerGlobalParams::FeatureCompatibility::Version::k32;
            } else {
                return Status(
                    ErrorCodes::BadValue,
                    str::stream()
                        << "Invalid value for "
                        << FeatureCompatibilityVersion::kVersionField
                        << ", found "
                        << elem.String()
                        << ", expected '"
                        << FeatureCompatibilityVersionCommandParser::kVersion34
                        << "' or '"
                        << FeatureCompatibilityVersionCommandParser::kVersion32
                        << "'. Contents of "
                        << FeatureCompatibilityVersion::kParameterName
                        << " document in "
                        << FeatureCompatibilityVersion::kCollection
                        << ": "
                        << featureCompatibilityVersionDoc
                        << ". See http://dochub.mongodb.org/core/3.4-feature-compatibility.");
            }
        } else {
            return Status(
                ErrorCodes::BadValue,
                str::stream() << "Unrecognized field '" << elem.fieldName() << "''. Contents of "
                              << FeatureCompatibilityVersion::kParameterName
                              << " document in "
                              << FeatureCompatibilityVersion::kCollection
                              << ": "
                              << featureCompatibilityVersionDoc
                              << ". See http://dochub.mongodb.org/core/3.4-feature-compatibility.");
        }
    }

    if (!foundVersionField) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Missing required field '"
                          << FeatureCompatibilityVersion::kVersionField
                          << "''. Contents of "
                          << FeatureCompatibilityVersion::kParameterName
                          << " document in "
                          << FeatureCompatibilityVersion::kCollection
                          << ": "
                          << featureCompatibilityVersionDoc
                          << ". See http://dochub.mongodb.org/core/3.4-feature-compatibility.");
    }

    return version;
}

void FeatureCompatibilityVersion::set(OperationContext* txn, StringData version) {
    uassert(40284,
            "featureCompatibilityVersion must be '3.4' or '3.2'. See "
            "http://dochub.mongodb.org/core/3.4-feature-compatibility.",
            version == FeatureCompatibilityVersionCommandParser::kVersion34 ||
                version == FeatureCompatibilityVersionCommandParser::kVersion32);

    DBDirectClient client(txn);
    NamespaceString nss(FeatureCompatibilityVersion::kCollection);

    if (version == FeatureCompatibilityVersionCommandParser::kVersion34) {
        // We build a v=2 index on the "admin.system.version" collection as part of setting the
        // featureCompatibilityVersion to 3.4. This is a new index version that isn't supported by
        // versions of MongoDB earlier than 3.4 that will cause 3.2 secondaries to crash when it is
        // replicated.
        std::vector<BSONObj> indexSpecs{k32IncompatibleIndexSpec};

        {
            ScopedTransaction transaction(txn, MODE_IX);
            AutoGetOrCreateDb autoDB(txn, nss.db(), MODE_X);

            uassert(ErrorCodes::NotMaster,
                    str::stream() << "Cannot set featureCompatibilityVersion to '" << version
                                  << "'. Not primary while attempting to create index on: "
                                  << nss.ns(),
                    repl::ReplicationCoordinator::get(txn->getServiceContext())
                        ->canAcceptWritesFor(nss));

            // If the "admin.system.version" collection has not been created yet, explicitly create
            // it to hold the v=2 index.
            if (!autoDB.getDb()->getCollection(nss)) {
                uassertStatusOK(repl::StorageInterface::get(txn)->createCollection(txn, nss, {}));
            }

            IndexBuilder builder(k32IncompatibleIndexSpec, false);
            auto status = builder.buildInForeground(txn, autoDB.getDb());
            uassertStatusOK(status);

            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                WriteUnitOfWork wuow(txn);
                getGlobalServiceContext()->getOpObserver()->onCreateIndex(
                    txn, autoDB.getDb()->getSystemIndexesName(), k32IncompatibleIndexSpec, false);
                wuow.commit();
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "FeatureCompatibilityVersion::set", nss.ns());
        }

        // We then update the featureCompatibilityVersion document stored in the
        // "admin.system.version" collection. We do this after creating the v=2 index in order to
        // maintain the invariant that if the featureCompatibilityVersion is 3.4, then
        // 'k32IncompatibleIndexSpec' index exists on the "admin.system.version" collection.
        BSONObj updateResult;
        client.runCommand(nss.db().toString(),
                          makeUpdateCommand(version, WriteConcernOptions::Majority),
                          updateResult);
        uassertStatusOK(getStatusFromWriteCommandReply(updateResult));

    } else if (version == FeatureCompatibilityVersionCommandParser::kVersion32) {
        // We update the featureCompatibilityVersion document stored in the "admin.system.version"
        // collection. We do this before dropping the v=2 index in order to maintain the invariant
        // that if the featureCompatibilityVersion is 3.4, then 'k32IncompatibleIndexSpec' index
        // exists on the "admin.system.version" collection. We don't attach a "majority" write
        // concern to this update because we're going to do so anyway for the "dropIndexes" command.
        BSONObj updateResult;
        client.runCommand(nss.db().toString(), makeUpdateCommand(version, BSONObj()), updateResult);
        uassertStatusOK(getStatusFromWriteCommandReply(updateResult));

        // We then drop the v=2 index on the "admin.system.version" collection to enable 3.2
        // secondaries to sync from this mongod.
        BSONObjBuilder dropIndexesCmd;
        dropIndexesCmd.append("dropIndexes", nss.coll());
        dropIndexesCmd.append("index", FeatureCompatibilityVersion::k32IncompatibleIndexName);
        dropIndexesCmd.append("writeConcern", WriteConcernOptions::Majority);

        BSONObj dropIndexesResult;
        client.runCommand(nss.db().toString(), dropIndexesCmd.done(), dropIndexesResult);
        auto status = getStatusFromCommandResult(dropIndexesResult);
        if (status != ErrorCodes::IndexNotFound) {
            uassertStatusOK(status);
        }
        uassertStatusOK(getWriteConcernStatusFromCommandResult(dropIndexesResult));
    }
}

void FeatureCompatibilityVersion::setIfCleanStartup(OperationContext* txn,
                                                    repl::StorageInterface* storageInterface) {
    if (serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
        std::vector<std::string> dbNames;
        StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
        storageEngine->listDatabases(&dbNames);

        for (auto&& dbName : dbNames) {
            if (dbName != "local") {
                return;
            }
        }

        UnreplicatedWritesBlock unreplicatedWritesBlock(txn);
        NamespaceString nss(FeatureCompatibilityVersion::kCollection);

        // We build a v=2 index on the "admin.system.version" collection as part of setting the
        // featureCompatibilityVersion to 3.4. This is a new index version that isn't supported by
        // versions of MongoDB earlier than 3.4 that will cause 3.2 secondaries to crash when it is
        // cloned.
        std::vector<BSONObj> indexSpecs{k32IncompatibleIndexSpec};

        {
            ScopedTransaction transaction(txn, MODE_IX);
            AutoGetOrCreateDb autoDB(txn, nss.db(), MODE_X);

            // We reached this point because the only database that exists on the server is "local"
            // and we have just created an empty "admin" database.
            // Therefore, it is safe to create the "admin.system.version" collection.
            invariant(autoDB.justCreated());
            uassertStatusOK(storageInterface->createCollection(txn, nss, {}));

            IndexBuilder builder(k32IncompatibleIndexSpec, false);
            auto status = builder.buildInForeground(txn, autoDB.getDb());
            uassertStatusOK(status);
        }

        // We then insert the featureCompatibilityVersion document into the "admin.system.version"
        // collection. The server parameter will be updated on commit by the op observer.
        // We do this after creating the v=2 index in order to maintain the invariant
        // that if the featureCompatibilityVersion is 3.4, then 'k32IncompatibleIndexSpec' index
        // exists on the "admin.system.version" collection. If we happened to fail to insert the
        // document when starting up, then on a subsequent start-up we'd no longer consider the data
        // files "clean" and would instead be in featureCompatibilityVersion=3.2.
        uassertStatusOK(storageInterface->insertDocument(
            txn,
            nss,
            BSON("_id" << FeatureCompatibilityVersion::kParameterName
                       << FeatureCompatibilityVersion::kVersionField
                       << FeatureCompatibilityVersionCommandParser::kVersion34)));
    }
}

void FeatureCompatibilityVersion::onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::String ||
        idElement.String() != FeatureCompatibilityVersion::kParameterName) {
        return;
    }
    auto newVersion = uassertStatusOK(FeatureCompatibilityVersion::parse(doc));
    log() << "setting featureCompatibilityVersion to "
          << getFeatureCompatibilityVersionString(newVersion);
    opCtx->recoveryUnit()->onCommit(
        [newVersion]() { serverGlobalParams.featureCompatibility.version.store(newVersion); });
}

void FeatureCompatibilityVersion::onDelete(OperationContext* opCtx, const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::String ||
        idElement.String() != FeatureCompatibilityVersion::kParameterName) {
        return;
    }
    log() << "setting featureCompatibilityVersion to "
          << FeatureCompatibilityVersionCommandParser::kVersion32;
    opCtx->recoveryUnit()->onCommit([]() {
        serverGlobalParams.featureCompatibility.version.store(
            ServerGlobalParams::FeatureCompatibility::Version::k32);
    });
}

void FeatureCompatibilityVersion::onDropCollection(OperationContext* opCtx) {
    log() << "setting featureCompatibilityVersion to "
          << FeatureCompatibilityVersionCommandParser::kVersion32;
    opCtx->recoveryUnit()->onCommit([]() {
        serverGlobalParams.featureCompatibility.version.store(
            ServerGlobalParams::FeatureCompatibility::Version::k32);
    });
}

/**
 * Read-only server parameter for featureCompatibilityVersion.
 */
class FeatureCompatibilityVersionParameter : public ServerParameter {
public:
    FeatureCompatibilityVersionParameter()
        : ServerParameter(ServerParameterSet::getGlobal(),
                          FeatureCompatibilityVersion::kParameterName.toString(),
                          false,  // allowedToChangeAtStartup
                          false   // allowedToChangeAtRuntime
                          ) {}

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        b.append(name,
                 getFeatureCompatibilityVersionString(
                     serverGlobalParams.featureCompatibility.version.load()));
    }

    virtual Status set(const BSONElement& newValueElement) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << FeatureCompatibilityVersion::kParameterName
                                    << " cannot be set via setParameter. See "
                                       "http://dochub.mongodb.org/core/3.4-feature-compatibility.");
    }

    virtual Status setFromString(const std::string& str) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << FeatureCompatibilityVersion::kParameterName
                                    << " cannot be set via setParameter. See "
                                       "http://dochub.mongodb.org/core/3.4-feature-compatibility.");
    }
} featureCompatibilityVersionParameter;

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(internalValidateFeaturesAsMaster, bool, true);

}  // namespace mongo
