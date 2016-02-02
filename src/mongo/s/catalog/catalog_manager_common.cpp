/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/catalog_manager_common.h"

#include <map>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/network_interface.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;

namespace {

const std::string kActionLogCollectionName("actionlog");
const int kActionLogCollectionSizeMB = 2 * 1024 * 1024;

const std::string kChangeLogCollectionName("changelog");
const int kChangeLogCollectionSizeMB = 10 * 1024 * 1024;

/**
 * Validates that the specified connection string can serve as a shard server. In particular,
 * this function checks that the shard can be contacted, that it is not already member of
 * another sharded cluster and etc.
 *
 * @param shardRegistry Shard registry to use for opening connections to the shards.
 * @param connectionString Connection string to be attempted as a shard host.
 * @param shardProposedName Optional proposed name for the shard. Can be omitted in which case
 *      a unique name for the shard will be generated from the shard's connection string. If it
 *      is not omitted, the value cannot be the empty string.
 *
 * On success returns a partially initialized shard type object corresponding to the requested
 * shard. It will have the hostName field set and optionally the name, if the name could be
 * generated from either the proposed name or the connection string set name. The returned
 * shard's name should be checked and if empty, one should be generated using some uniform
 * algorithm.
 */
StatusWith<ShardType> validateHostAsShard(OperationContext* txn,
                                          ShardRegistry* shardRegistry,
                                          const ConnectionString& connectionString,
                                          const std::string* shardProposedName) {
    if (connectionString.type() == ConnectionString::SYNC) {
        return {ErrorCodes::BadValue,
                "can't use sync cluster as a shard; for a replica set, "
                "you have to use <setname>/<server1>,<server2>,..."};
    }

    if (shardProposedName && shardProposedName->empty()) {
        return {ErrorCodes::BadValue, "shard name cannot be empty"};
    }

    const std::shared_ptr<Shard> shardConn{shardRegistry->createConnection(connectionString)};
    invariant(shardConn);

    const ReadPreferenceSetting readPref{ReadPreference::PrimaryOnly};

    // Is it mongos?
    auto cmdStatus = shardRegistry->runCommandForAddShard(
        txn, shardConn, readPref, "admin", BSON("isdbgrid" << 1));
    if (!cmdStatus.isOK()) {
        return cmdStatus.getStatus();
    }

    // (ok == 1) implies that it is a mongos
    if (getStatusFromCommandResult(cmdStatus.getValue()).isOK()) {
        return {ErrorCodes::OperationFailed, "can't add a mongos process as a shard"};
    }

    // Is it a replica set?
    cmdStatus = shardRegistry->runCommandForAddShard(
        txn, shardConn, readPref, "admin", BSON("isMaster" << 1));
    if (!cmdStatus.isOK()) {
        return cmdStatus.getStatus();
    }

    BSONObj resIsMaster = cmdStatus.getValue();

    const string providedSetName = connectionString.getSetName();
    const string foundSetName = resIsMaster["setName"].str();

    // Make sure the specified replica set name (if any) matches the actual shard's replica set
    if (providedSetName.empty() && !foundSetName.empty()) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "host is part of set " << foundSetName << "; "
                              << "use replica set url format "
                              << "<setname>/<server1>,<server2>, ..."};
    }

    if (!providedSetName.empty() && foundSetName.empty()) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "host did not return a set name; "
                              << "is the replica set still initializing? " << resIsMaster};
    }

    // Make sure the set name specified in the connection string matches the one where its hosts
    // belong into
    if (!providedSetName.empty() && (providedSetName != foundSetName)) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "the provided connection string (" << connectionString.toString()
                              << ") does not match the actual set name " << foundSetName};
    }

    // Is it a mongos config server?
    cmdStatus = shardRegistry->runCommandForAddShard(
        txn, shardConn, readPref, "admin", BSON("replSetGetStatus" << 1));
    if (!cmdStatus.isOK()) {
        return cmdStatus.getStatus();
    }

    BSONObj res = cmdStatus.getValue();

    if (getStatusFromCommandResult(res).isOK()) {
        bool isConfigServer;
        Status status =
            bsonExtractBooleanFieldWithDefault(res, "configsvr", false, &isConfigServer);
        if (!status.isOK()) {
            return Status(status.code(),
                          str::stream() << "replSetGetStatus returned invalid \"configsvr\" "
                                        << "field when attempting to add "
                                        << connectionString.toString()
                                        << " as a shard: " << status.reason());
        }

        if (isConfigServer) {
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Cannot add " << connectionString.toString()
                                  << " as a shard since it is part of a config server replica set"};
        }
    } else if ((res["info"].type() == String) && (res["info"].String() == "configsvr")) {
        return {ErrorCodes::OperationFailed,
                "the specified mongod is a legacy-style config "
                "server and cannot be used as a shard server"};
    }

    // If the shard is part of a replica set, make sure all the hosts mentioned in the connection
    // string are part of the set. It is fine if not all members of the set are mentioned in the
    // connection string, though.
    if (!providedSetName.empty()) {
        std::set<string> hostSet;

        BSONObjIterator iter(resIsMaster["hosts"].Obj());
        while (iter.more()) {
            hostSet.insert(iter.next().String());  // host:port
        }

        if (resIsMaster["passives"].isABSONObj()) {
            BSONObjIterator piter(resIsMaster["passives"].Obj());
            while (piter.more()) {
                hostSet.insert(piter.next().String());  // host:port
            }
        }

        if (resIsMaster["arbiters"].isABSONObj()) {
            BSONObjIterator piter(resIsMaster["arbiters"].Obj());
            while (piter.more()) {
                hostSet.insert(piter.next().String());  // host:port
            }
        }

        vector<HostAndPort> hosts = connectionString.getServers();
        for (size_t i = 0; i < hosts.size(); i++) {
            const string host = hosts[i].toString();  // host:port
            if (hostSet.find(host) == hostSet.end()) {
                return {ErrorCodes::OperationFailed,
                        str::stream() << "in seed list " << connectionString.toString() << ", host "
                                      << host << " does not belong to replica set " << foundSetName
                                      << "; found " << resIsMaster.toString()};
            }
        }
    }

    string actualShardName;

    if (shardProposedName) {
        actualShardName = *shardProposedName;
    } else if (!foundSetName.empty()) {
        // Default it to the name of the replica set
        actualShardName = foundSetName;
    }

    // Disallow adding shard replica set with name 'config'
    if (actualShardName == "config") {
        return {ErrorCodes::BadValue, "use of shard replica set with name 'config' is not allowed"};
    }

    // Retrieve the most up to date connection string that we know from the replica set monitor (if
    // this is a replica set shard, otherwise it will be the same value as connectionString).
    ConnectionString actualShardConnStr = shardConn->getTargeter()->connectionString();

    ShardType shard;
    shard.setName(actualShardName);
    shard.setHost(actualShardConnStr.toString());

    return shard;
}

/**
 * Runs the listDatabases command on the specified host and returns the names of all databases
 * it returns excluding those named local and admin, since they serve administrative purpose.
 */
StatusWith<std::vector<std::string>> getDBNamesListFromShard(
    OperationContext* txn, ShardRegistry* shardRegistry, const ConnectionString& connectionString) {
    const std::shared_ptr<Shard> shardConn{
        shardRegistry->createConnection(connectionString).release()};
    invariant(shardConn);

    auto cmdStatus =
        shardRegistry->runCommandForAddShard(txn,
                                             shardConn,
                                             ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                             "admin",
                                             BSON("listDatabases" << 1));
    if (!cmdStatus.isOK()) {
        return cmdStatus.getStatus();
    }

    const BSONObj& cmdResult = cmdStatus.getValue();

    Status cmdResultStatus = getStatusFromCommandResult(cmdResult);
    if (!cmdResultStatus.isOK()) {
        return cmdResultStatus;
    }

    vector<string> dbNames;

    for (const auto& dbEntry : cmdResult["databases"].Obj()) {
        const string& dbName = dbEntry["name"].String();

        if (!(dbName == "local" || dbName == "admin")) {
            dbNames.push_back(dbName);
        }
    }

    return dbNames;
}

}  // namespace

StatusWith<string> CatalogManagerCommon::addShard(OperationContext* txn,
                                                  const std::string* shardProposedName,
                                                  const ConnectionString& shardConnectionString,
                                                  const long long maxSize) {
    // Validate the specified connection string may serve as shard at all
    auto shardStatus =
        validateHostAsShard(txn, grid.shardRegistry(), shardConnectionString, shardProposedName);
    if (!shardStatus.isOK()) {
        // TODO: This is a workaround for the case were we could have some bad shard being
        // requested to be added and we put that bad connection string on the global replica set
        // monitor registry. It needs to be cleaned up so that when a correct replica set is added,
        // it will be recreated.
        ReplicaSetMonitor::remove(shardConnectionString.getSetName());
        return shardStatus.getStatus();
    }

    ShardType& shardType = shardStatus.getValue();

    auto dbNamesStatus = getDBNamesListFromShard(txn, grid.shardRegistry(), shardConnectionString);
    if (!dbNamesStatus.isOK()) {
        return dbNamesStatus.getStatus();
    }

    // Check that none of the existing shard candidate's dbs exist already
    for (const string& dbName : dbNamesStatus.getValue()) {
        auto dbt = getDatabase(txn, dbName);
        if (dbt.isOK()) {
            const auto& dbDoc = dbt.getValue().value;
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "can't add shard "
                                        << "'" << shardConnectionString.toString() << "'"
                                        << " because a local database '" << dbName
                                        << "' exists in another " << dbDoc.getPrimary());
        } else if (dbt != ErrorCodes::NamespaceNotFound) {
            return dbt.getStatus();
        }
    }

    // If a name for a shard wasn't provided, generate one
    if (shardType.getName().empty()) {
        StatusWith<string> result = _generateNewShardName(txn);
        if (!result.isOK()) {
            return Status(ErrorCodes::OperationFailed, "error generating new shard name");
        }

        shardType.setName(result.getValue());
    }

    if (maxSize > 0) {
        shardType.setMaxSizeMB(maxSize);
    }

    log() << "going to add shard: " << shardType.toString();

    Status result = insertConfigDocument(txn, ShardType::ConfigNS, shardType.toBSON());
    if (!result.isOK()) {
        log() << "error adding shard: " << shardType.toBSON() << " err: " << result.reason();
        return result;
    }

    // Make sure the new shard is visible
    grid.shardRegistry()->reload(txn);

    // Add all databases which were discovered on the new shard
    for (const string& dbName : dbNamesStatus.getValue()) {
        DatabaseType dbt;
        dbt.setName(dbName);
        dbt.setPrimary(shardType.getName());
        dbt.setSharded(false);

        Status status = updateDatabase(txn, dbName, dbt);
        if (!status.isOK()) {
            log() << "adding shard " << shardConnectionString.toString()
                  << " even though could not add database " << dbName;
        }
    }

    // Record in changelog
    BSONObjBuilder shardDetails;
    shardDetails.append("name", shardType.getName());
    shardDetails.append("host", shardConnectionString.toString());

    logChange(txn, "addShard", "", shardDetails.obj());

    return shardType.getName();
}

Status CatalogManagerCommon::updateCollection(OperationContext* txn,
                                              const std::string& collNs,
                                              const CollectionType& coll) {
    fassert(28634, coll.validate());

    auto status = updateConfigDocument(
        txn, CollectionType::ConfigNS, BSON(CollectionType::fullNs(collNs)), coll.toBSON(), true);
    if (!status.isOK()) {
        return Status(status.getStatus().code(),
                      str::stream() << "collection metadata write failed"
                                    << causedBy(status.getStatus()));
    }

    return Status::OK();
}

Status CatalogManagerCommon::updateDatabase(OperationContext* txn,
                                            const std::string& dbName,
                                            const DatabaseType& db) {
    fassert(28616, db.validate());

    auto status = updateConfigDocument(
        txn, DatabaseType::ConfigNS, BSON(DatabaseType::name(dbName)), db.toBSON(), true);
    if (!status.isOK()) {
        return Status(status.getStatus().code(),
                      str::stream() << "database metadata write failed"
                                    << causedBy(status.getStatus()));
    }

    return Status::OK();
}

Status CatalogManagerCommon::createDatabase(OperationContext* txn, const std::string& dbName) {
    invariant(nsIsDbOnly(dbName));

    // The admin and config databases should never be explicitly created. They "just exist",
    // i.e. getDatabase will always return an entry for them.
    invariant(dbName != "admin");
    invariant(dbName != "config");

    // Lock the database globally to prevent conflicts with simultaneous database creation.
    auto scopedDistLock = getDistLockManager()->lock(txn, dbName, "createDatabase");
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    // check for case sensitivity violations
    Status status = _checkDbDoesNotExist(txn, dbName, nullptr);
    if (!status.isOK()) {
        return status;
    }

    // Database does not exist, pick a shard and create a new entry
    auto newShardIdStatus = selectShardForNewDatabase(txn, grid.shardRegistry());
    if (!newShardIdStatus.isOK()) {
        return newShardIdStatus.getStatus();
    }

    const ShardId& newShardId = newShardIdStatus.getValue();

    log() << "Placing [" << dbName << "] on: " << newShardId;

    DatabaseType db;
    db.setName(dbName);
    db.setPrimary(newShardId);
    db.setSharded(false);

    status = insertConfigDocument(txn, DatabaseType::ConfigNS, db.toBSON());
    if (status.code() == ErrorCodes::DuplicateKey) {
        return Status(ErrorCodes::NamespaceExists, "database " + dbName + " already exists");
    }

    return status;
}

Status CatalogManagerCommon::logAction(OperationContext* txn,
                                       const std::string& what,
                                       const std::string& ns,
                                       const BSONObj& detail) {
    if (_actionLogCollectionCreated.load() == 0) {
        Status result = _createCappedConfigCollection(
            txn, kActionLogCollectionName, kActionLogCollectionSizeMB);
        if (result.isOK() || result == ErrorCodes::NamespaceExists) {
            _actionLogCollectionCreated.store(1);
        } else {
            log() << "couldn't create config.actionlog collection:" << causedBy(result);
            return result;
        }
    }

    return _log(txn, kActionLogCollectionName, what, ns, detail);
}

Status CatalogManagerCommon::logChange(OperationContext* txn,
                                       const std::string& what,
                                       const std::string& ns,
                                       const BSONObj& detail) {
    if (_changeLogCollectionCreated.load() == 0) {
        Status result = _createCappedConfigCollection(
            txn, kChangeLogCollectionName, kChangeLogCollectionSizeMB);
        if (result.isOK() || result == ErrorCodes::NamespaceExists) {
            _changeLogCollectionCreated.store(1);
        } else {
            log() << "couldn't create config.changelog collection:" << causedBy(result);
            return result;
        }
    }

    return _log(txn, kChangeLogCollectionName, what, ns, detail);
}

// static
StatusWith<ShardId> CatalogManagerCommon::selectShardForNewDatabase(OperationContext* txn,
                                                                    ShardRegistry* shardRegistry) {
    vector<ShardId> allShardIds;

    shardRegistry->getAllShardIds(&allShardIds);
    if (allShardIds.empty()) {
        shardRegistry->reload(txn);
        shardRegistry->getAllShardIds(&allShardIds);

        if (allShardIds.empty()) {
            return Status(ErrorCodes::ShardNotFound, "No shards found");
        }
    }

    ShardId candidateShardId = allShardIds[0];

    auto candidateSizeStatus =
        shardutil::retrieveTotalShardSize(txn, candidateShardId, shardRegistry);
    if (!candidateSizeStatus.isOK()) {
        return candidateSizeStatus.getStatus();
    }

    for (size_t i = 1; i < allShardIds.size(); i++) {
        const ShardId shardId = allShardIds[i];

        const auto sizeStatus = shardutil::retrieveTotalShardSize(txn, shardId, shardRegistry);
        if (!sizeStatus.isOK()) {
            return sizeStatus.getStatus();
        }

        if (sizeStatus.getValue() < candidateSizeStatus.getValue()) {
            candidateSizeStatus = sizeStatus;
            candidateShardId = shardId;
        }
    }

    return candidateShardId;
}

Status CatalogManagerCommon::enableSharding(OperationContext* txn, const std::string& dbName) {
    invariant(nsIsDbOnly(dbName));

    DatabaseType db;

    // Lock the database globally to prevent conflicts with simultaneous database
    // creation/modification.
    auto scopedDistLock = getDistLockManager()->lock(txn, dbName, "enableSharding");
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    // Check for case sensitivity violations
    Status status = _checkDbDoesNotExist(txn, dbName, &db);
    if (status.isOK()) {
        // Database does not exist, create a new entry
        auto newShardIdStatus = selectShardForNewDatabase(txn, grid.shardRegistry());
        if (!newShardIdStatus.isOK()) {
            return newShardIdStatus.getStatus();
        }

        const ShardId& newShardId = newShardIdStatus.getValue();

        log() << "Placing [" << dbName << "] on: " << newShardId;

        db.setName(dbName);
        db.setPrimary(newShardId);
        db.setSharded(true);
    } else if (status.code() == ErrorCodes::NamespaceExists) {
        if (db.getSharded()) {
            return Status(ErrorCodes::AlreadyInitialized,
                          str::stream() << "sharding already enabled for database " << dbName);
        }

        // Database exists, so just update it
        db.setSharded(true);
    } else {
        return status;
    }

    log() << "Enabling sharding for database [" << dbName << "] in config db";

    return updateDatabase(txn, dbName, db);
}

Status CatalogManagerCommon::_log(OperationContext* txn,
                                  const StringData& logCollName,
                                  const std::string& what,
                                  const std::string& operationNS,
                                  const BSONObj& detail) {
    Date_t now = grid.shardRegistry()->getExecutor()->now();
    const std::string hostName = grid.shardRegistry()->getNetwork()->getHostName();
    const string changeId = str::stream() << hostName << "-" << now.toString() << "-" << OID::gen();

    ChangeLogType changeLog;
    changeLog.setChangeId(changeId);
    changeLog.setServer(hostName);
    changeLog.setClientAddr(txn->getClient()->clientAddress(true));
    changeLog.setTime(now);
    changeLog.setNS(operationNS);
    changeLog.setWhat(what);
    changeLog.setDetails(detail);

    BSONObj changeLogBSON = changeLog.toBSON();
    log() << "about to log metadata event into " << logCollName << ": " << changeLogBSON;

    const NamespaceString nss("config", logCollName);
    Status result = insertConfigDocument(txn, nss.ns(), changeLogBSON);
    if (!result.isOK()) {
        warning() << "Error encountered while logging config change with ID [" << changeId
                  << "] into collection " << logCollName << ": " << result;
    }

    return result;
}

}  // namespace mongo
