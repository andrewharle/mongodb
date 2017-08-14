/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/s/catalog/sharding_catalog_manager_impl.h"

#include <iomanip>
#include <set>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_command_parser.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using CallbackHandle = executor::TaskExecutor::CallbackHandle;
using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using RemoteCommandCallbackFn = executor::TaskExecutor::RemoteCommandCallbackFn;

const Seconds kDefaultFindHostMaxWaitTime(20);

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

MONGO_FP_DECLARE(dontUpsertShardIdentityOnNewShards);

/**
 * Generates a unique name to be given to a newly added shard.
 */
StatusWith<std::string> generateNewShardName(OperationContext* txn) {
    BSONObjBuilder shardNameRegex;
    shardNameRegex.appendRegex(ShardType::name(), "^shard");

    auto findStatus = Grid::get(txn)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        txn,
        kConfigReadSelector,
        repl::ReadConcernLevel::kMajorityReadConcern,
        NamespaceString(ShardType::ConfigNS),
        shardNameRegex.obj(),
        BSON(ShardType::name() << -1),
        1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().docs;

    int count = 0;
    if (!docs.empty()) {
        const auto shardStatus = ShardType::fromBSON(docs.front());
        if (!shardStatus.isOK()) {
            return shardStatus.getStatus();
        }

        std::istringstream is(shardStatus.getValue().getName().substr(5));
        is >> count;
        count++;
    }

    // TODO: fix so that we can have more than 10000 automatically generated shard names
    if (count < 9999) {
        std::stringstream ss;
        ss << "shard" << std::setfill('0') << std::setw(4) << count;
        return ss.str();
    }

    return Status(ErrorCodes::OperationFailed, "unable to generate new shard name");
}

}  // namespace

StatusWith<Shard::CommandResponse> ShardingCatalogManagerImpl::_runCommandForAddShard(
    OperationContext* txn,
    RemoteCommandTargeter* targeter,
    const std::string& dbName,
    const BSONObj& cmdObj) {
    auto host = targeter->findHost(txn, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
    if (!host.isOK()) {
        return host.getStatus();
    }

    executor::RemoteCommandRequest request(
        host.getValue(), dbName, cmdObj, rpc::makeEmptyMetadata(), nullptr, Seconds(30));
    executor::RemoteCommandResponse swResponse =
        Status(ErrorCodes::InternalError, "Internal error running command");

    auto callStatus = _executorForAddShard->scheduleRemoteCommand(
        request, [&swResponse](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            swResponse = args.response;
        });
    if (!callStatus.isOK()) {
        return callStatus.getStatus();
    }

    // Block until the command is carried out
    _executorForAddShard->wait(callStatus.getValue());

    if (!swResponse.isOK()) {
        if (swResponse.status.compareCode(ErrorCodes::ExceededTimeLimit)) {
            LOG(0) << "Operation for addShard timed out with status " << swResponse.status;
        }
        if (!Shard::shouldErrorBePropagated(swResponse.status.code())) {
            swResponse.status = {ErrorCodes::OperationFailed,
                                 str::stream() << "failed to run command " << cmdObj
                                               << " when attempting to add shard "
                                               << targeter->connectionString().toString()
                                               << causedBy(swResponse.status)};
        }
        return swResponse.status;
    }

    BSONObj responseObj = swResponse.data.getOwned();
    BSONObj responseMetadata = swResponse.metadata.getOwned();

    Status commandStatus = getStatusFromCommandResult(responseObj);
    if (!Shard::shouldErrorBePropagated(commandStatus.code())) {
        commandStatus = {ErrorCodes::OperationFailed,
                         str::stream() << "failed to run command " << cmdObj
                                       << " when attempting to add shard "
                                       << targeter->connectionString().toString()
                                       << causedBy(commandStatus)};
    }

    Status writeConcernStatus = getWriteConcernStatusFromCommandResult(responseObj);
    if (!Shard::shouldErrorBePropagated(writeConcernStatus.code())) {
        writeConcernStatus = {ErrorCodes::OperationFailed,
                              str::stream() << "failed to satisfy writeConcern for command "
                                            << cmdObj
                                            << " when attempting to add shard "
                                            << targeter->connectionString().toString()
                                            << causedBy(writeConcernStatus)};
    }

    return Shard::CommandResponse(std::move(responseObj),
                                  std::move(responseMetadata),
                                  std::move(commandStatus),
                                  std::move(writeConcernStatus));
}

StatusWith<boost::optional<ShardType>> ShardingCatalogManagerImpl::_checkIfShardExists(
    OperationContext* txn,
    const ConnectionString& proposedShardConnectionString,
    const std::string* proposedShardName,
    long long proposedShardMaxSize) {
    // Check whether any host in the connection is already part of the cluster.
    const auto existingShards = Grid::get(txn)->catalogClient(txn)->getAllShards(
        txn, repl::ReadConcernLevel::kLocalReadConcern);
    if (!existingShards.isOK()) {
        return Status(existingShards.getStatus().code(),
                      str::stream() << "Failed to load existing shards during addShard"
                                    << causedBy(existingShards.getStatus().reason()));
    }

    // Now check if this shard already exists - if it already exists *with the same options* then
    // the addShard request can return success early without doing anything more.
    for (const auto& existingShard : existingShards.getValue().value) {
        auto swExistingShardConnStr = ConnectionString::parse(existingShard.getHost());
        if (!swExistingShardConnStr.isOK()) {
            return swExistingShardConnStr.getStatus();
        }
        auto existingShardConnStr = std::move(swExistingShardConnStr.getValue());

        // Function for determining if the options for the shard that is being added match the
        // options of an existing shard that conflicts with it.
        auto shardsAreEquivalent = [&]() {
            if (proposedShardName && *proposedShardName != existingShard.getName()) {
                return false;
            }
            if (proposedShardConnectionString.type() != existingShardConnStr.type()) {
                return false;
            }
            if (proposedShardConnectionString.type() == ConnectionString::SET &&
                proposedShardConnectionString.getSetName() != existingShardConnStr.getSetName()) {
                return false;
            }
            if (proposedShardMaxSize != existingShard.getMaxSizeMB()) {
                return false;
            }
            return true;
        };

        if (existingShardConnStr.type() == ConnectionString::SET &&
            proposedShardConnectionString.type() == ConnectionString::SET &&
            existingShardConnStr.getSetName() == proposedShardConnectionString.getSetName()) {
            // An existing shard has the same replica set name as the shard being added.
            // If the options aren't the same, then this is an error,
            // but if the options match then the addShard operation should be immediately
            // considered a success and terminated.
            if (shardsAreEquivalent()) {
                return {existingShard};
            } else {
                return {ErrorCodes::IllegalOperation,
                        str::stream() << "A shard already exists containing the replica set '"
                                      << existingShardConnStr.getSetName()
                                      << "'"};
            }
        }

        for (const auto& existingHost : existingShardConnStr.getServers()) {
            // Look if any of the hosts in the existing shard are present within the shard trying
            // to be added.
            for (const auto& addingHost : proposedShardConnectionString.getServers()) {
                if (existingHost == addingHost) {
                    // At least one of the hosts in the shard being added already exists in an
                    // existing shard.  If the options aren't the same, then this is an error,
                    // but if the options match then the addShard operation should be immediately
                    // considered a success and terminated.
                    if (shardsAreEquivalent()) {
                        return {existingShard};
                    } else {
                        return {ErrorCodes::IllegalOperation,
                                str::stream() << "'" << addingHost.toString() << "' "
                                              << "is already a member of the existing shard '"
                                              << existingShard.getHost()
                                              << "' ("
                                              << existingShard.getName()
                                              << ")."};
                    }
                }
            }
        }

        if (proposedShardName && *proposedShardName == existingShard.getName()) {
            // If we get here then we're trying to add a shard with the same name as an existing
            // shard, but there was no overlap in the hosts between the existing shard and the
            // proposed connection string for the new shard.
            return {ErrorCodes::IllegalOperation,
                    str::stream() << "A shard named " << *proposedShardName << " already exists"};
        }
    }

    return {boost::none};
}

StatusWith<ShardType> ShardingCatalogManagerImpl::_validateHostAsShard(
    OperationContext* txn,
    std::shared_ptr<RemoteCommandTargeter> targeter,
    const std::string* shardProposedName,
    const ConnectionString& connectionString) {

    // Check if the node being added is a mongos or a version of mongod too old to speak the current
    // communication protocol.
    auto swCommandResponse =
        _runCommandForAddShard(txn, targeter.get(), "admin", BSON("isMaster" << 1));
    if (!swCommandResponse.isOK()) {
        if (swCommandResponse.getStatus() == ErrorCodes::RPCProtocolNegotiationFailed) {
            // Mongos to mongos commands are no longer supported in the wire protocol
            // (because mongos does not support OP_COMMAND), similarly for a new mongos
            // and an old mongod. So the call will fail in such cases.
            // TODO: If/When mongos ever supports opCommands, this logic will break because
            // cmdStatus will be OK.
            return {ErrorCodes::RPCProtocolNegotiationFailed,
                    str::stream() << targeter->connectionString().toString()
                                  << " does not recognize the RPC protocol being used. This is"
                                  << " likely because it contains a node that is a mongos or an old"
                                  << " version of mongod."};
        } else {
            return swCommandResponse.getStatus();
        }
    }

    // Check for a command response error
    auto resIsMasterStatus = std::move(swCommandResponse.getValue().commandStatus);
    if (!resIsMasterStatus.isOK()) {
        return {resIsMasterStatus.code(),
                str::stream() << "Error running isMaster against "
                              << targeter->connectionString().toString()
                              << ": "
                              << causedBy(resIsMasterStatus)};
    }

    auto resIsMaster = std::move(swCommandResponse.getValue().response);

    // Check that the node being added is a new enough version.
    // If we're running this code, that means the mongos that the addShard request originated from
    // must be at least version 3.4 (since 3.2 mongoses don't know about the _configsvrAddShard
    // command).  Since it is illegal to have v3.4 mongoses with v3.2 shards, we should reject
    // adding any shards that are not v3.4.  We can determine this by checking that the
    // maxWireVersion reported in isMaster is at least COMMANDS_ACCEPT_WRITE_CONCERN.
    // TODO(SERVER-25623): This approach won't work to prevent v3.6 mongoses from adding v3.4
    // shards, so we'll have to rethink this during the 3.5 development cycle.

    long long maxWireVersion;
    Status status = bsonExtractIntegerField(resIsMaster, "maxWireVersion", &maxWireVersion);
    if (!status.isOK()) {
        return Status(status.code(),
                      str::stream() << "isMaster returned invalid 'maxWireVersion' "
                                    << "field when attempting to add "
                                    << connectionString.toString()
                                    << " as a shard: "
                                    << status.reason());
    }
    if (maxWireVersion < WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN) {
        return Status(ErrorCodes::IncompatibleServerVersion,
                      str::stream() << "Cannot add " << connectionString.toString()
                                    << " as a shard because we detected a mongod with server "
                                       "version older than 3.4.0.  It is invalid to add v3.2 and "
                                       "older shards through a v3.4 mongos.");
    }


    // Check whether there is a master. If there isn't, the replica set may not have been
    // initiated. If the connection is a standalone, it will return true for isMaster.
    bool isMaster;
    status = bsonExtractBooleanField(resIsMaster, "ismaster", &isMaster);
    if (!status.isOK()) {
        return Status(status.code(),
                      str::stream() << "isMaster returned invalid 'ismaster' "
                                    << "field when attempting to add "
                                    << connectionString.toString()
                                    << " as a shard: "
                                    << status.reason());
    }
    if (!isMaster) {
        return {ErrorCodes::NotMaster,
                str::stream()
                    << connectionString.toString()
                    << " does not have a master. If this is a replica set, ensure that it has a"
                    << " healthy primary and that the set has been properly initiated."};
    }

    const std::string providedSetName = connectionString.getSetName();
    const std::string foundSetName = resIsMaster["setName"].str();

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
                              << "is the replica set still initializing? "
                              << resIsMaster};
    }

    // Make sure the set name specified in the connection string matches the one where its hosts
    // belong into
    if (!providedSetName.empty() && (providedSetName != foundSetName)) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "the provided connection string (" << connectionString.toString()
                              << ") does not match the actual set name "
                              << foundSetName};
    }

    // Is it a config server?
    if (resIsMaster.hasField("configsvr")) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "Cannot add " << connectionString.toString()
                              << " as a shard since it is a config server"};
    }

    // If the shard is part of a replica set, make sure all the hosts mentioned in the connection
    // string are part of the set. It is fine if not all members of the set are mentioned in the
    // connection string, though.
    if (!providedSetName.empty()) {
        std::set<std::string> hostSet;

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

        for (const auto& hostEntry : connectionString.getServers()) {
            const auto& host = hostEntry.toString();  // host:port
            if (hostSet.find(host) == hostSet.end()) {
                return {ErrorCodes::OperationFailed,
                        str::stream() << "in seed list " << connectionString.toString() << ", host "
                                      << host
                                      << " does not belong to replica set "
                                      << foundSetName
                                      << "; found "
                                      << resIsMaster.toString()};
            }
        }
    }

    std::string actualShardName;

    if (shardProposedName) {
        actualShardName = *shardProposedName;
    } else if (!foundSetName.empty()) {
        // Default it to the name of the replica set
        actualShardName = foundSetName;
    }

    // Disallow adding shard replica set with name 'config'
    if (actualShardName == NamespaceString::kConfigDb) {
        return {ErrorCodes::BadValue, "use of shard replica set with name 'config' is not allowed"};
    }

    // Retrieve the most up to date connection string that we know from the replica set monitor (if
    // this is a replica set shard, otherwise it will be the same value as connectionString).
    ConnectionString actualShardConnStr = targeter->connectionString();

    ShardType shard;
    shard.setName(actualShardName);
    shard.setHost(actualShardConnStr.toString());
    shard.setState(ShardType::ShardState::kShardAware);

    return shard;
}

StatusWith<std::vector<std::string>> ShardingCatalogManagerImpl::_getDBNamesListFromShard(
    OperationContext* txn, std::shared_ptr<RemoteCommandTargeter> targeter) {

    auto swCommandResponse = _runCommandForAddShard(
        txn, targeter.get(), "admin", BSON("listDatabases" << 1 << "nameOnly" << true));
    if (!swCommandResponse.isOK()) {
        return swCommandResponse.getStatus();
    }

    auto cmdStatus = std::move(swCommandResponse.getValue().commandStatus);
    if (!cmdStatus.isOK()) {
        return cmdStatus;
    }

    auto cmdResult = std::move(swCommandResponse.getValue().response);

    std::vector<std::string> dbNames;

    for (const auto& dbEntry : cmdResult["databases"].Obj()) {
        const auto& dbName = dbEntry["name"].String();

        if (!(dbName == NamespaceString::kAdminDb || dbName == NamespaceString::kLocalDb)) {
            dbNames.push_back(dbName);
        }
    }

    return dbNames;
}

StatusWith<std::string> ShardingCatalogManagerImpl::addShard(
    OperationContext* txn,
    const std::string* shardProposedName,
    const ConnectionString& shardConnectionString,
    const long long maxSize) {
    if (shardConnectionString.type() == ConnectionString::INVALID) {
        return {ErrorCodes::BadValue, "Invalid connection string"};
    }

    if (shardProposedName && shardProposedName->empty()) {
        return {ErrorCodes::BadValue, "shard name cannot be empty"};
    }

    // Only one addShard operation can be in progress at a time.
    Lock::ExclusiveLock lk(txn->lockState(), _kShardMembershipLock);

    // Check if this shard has already been added (can happen in the case of a retry after a network
    // error, for example) and thus this addShard request should be considered a no-op.
    auto existingShard =
        _checkIfShardExists(txn, shardConnectionString, shardProposedName, maxSize);
    if (!existingShard.isOK()) {
        return existingShard.getStatus();
    }
    if (existingShard.getValue()) {
        // These hosts already belong to an existing shard, so report success and terminate the
        // addShard request.  Make sure to set the last optime for the client to the system last
        // optime so that we'll still wait for replication so that this state is visible in the
        // committed snapshot.
        repl::ReplClientInfo::forClient(txn->getClient()).setLastOpToSystemLastOpTime(txn);
        return existingShard.getValue()->getName();
    }

    // Force a reload of the ShardRegistry to ensure that, in case this addShard is to re-add a
    // replica set that has recently been removed, we have detached the ReplicaSetMonitor for the
    // set with that setName from the ReplicaSetMonitorManager and will create a new
    // ReplicaSetMonitor when targeting the set below.
    // Note: This is necessary because as of 3.4, removeShard is performed by mongos (unlike
    // addShard), so the ShardRegistry is not synchronously reloaded on the config server when a
    // shard is removed.
    if (!Grid::get(txn)->shardRegistry()->reload(txn)) {
        // If the first reload joined an existing one, call reload again to ensure the reload is
        // fresh.
        Grid::get(txn)->shardRegistry()->reload(txn);
    }

    // TODO: Don't create a detached Shard object, create a detached RemoteCommandTargeter instead.
    const std::shared_ptr<Shard> shard{
        Grid::get(txn)->shardRegistry()->createConnection(shardConnectionString)};
    invariant(shard);
    auto targeter = shard->getTargeter();

    auto stopMonitoringGuard = MakeGuard([&] {
        if (shardConnectionString.type() == ConnectionString::SET) {
            // This is a workaround for the case were we could have some bad shard being
            // requested to be added and we put that bad connection string on the global replica set
            // monitor registry. It needs to be cleaned up so that when a correct replica set is
            // added, it will be recreated.
            ReplicaSetMonitor::remove(shardConnectionString.getSetName());
        }
    });

    // Validate the specified connection string may serve as shard at all
    auto shardStatus =
        _validateHostAsShard(txn, targeter, shardProposedName, shardConnectionString);
    if (!shardStatus.isOK()) {
        return shardStatus.getStatus();
    }
    ShardType& shardType = shardStatus.getValue();

    // Check that none of the existing shard candidate's dbs exist already
    auto dbNamesStatus = _getDBNamesListFromShard(txn, targeter);
    if (!dbNamesStatus.isOK()) {
        return dbNamesStatus.getStatus();
    }

    for (const auto& dbName : dbNamesStatus.getValue()) {
        auto dbt = Grid::get(txn)->catalogClient(txn)->getDatabase(txn, dbName);
        if (dbt.isOK()) {
            const auto& dbDoc = dbt.getValue().value;
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "can't add shard "
                                        << "'"
                                        << shardConnectionString.toString()
                                        << "'"
                                        << " because a local database '"
                                        << dbName
                                        << "' exists in another "
                                        << dbDoc.getPrimary());
        } else if (dbt != ErrorCodes::NamespaceNotFound) {
            return dbt.getStatus();
        }
    }

    // If a name for a shard wasn't provided, generate one
    if (shardType.getName().empty()) {
        auto result = generateNewShardName(txn);
        if (!result.isOK()) {
            return result.getStatus();
        }
        shardType.setName(result.getValue());
    }

    if (maxSize > 0) {
        shardType.setMaxSizeMB(maxSize);
    }

    // If the minimum allowed version for the cluster is 3.4, set the featureCompatibilityVersion to
    // 3.4 on the shard.
    if (serverGlobalParams.featureCompatibility.version.load() ==
        ServerGlobalParams::FeatureCompatibility::Version::k34) {
        auto versionResponse =
            _runCommandForAddShard(txn,
                                   targeter.get(),
                                   "admin",
                                   BSON(FeatureCompatibilityVersion::kCommandName
                                        << FeatureCompatibilityVersionCommandParser::kVersion34));
        if (!versionResponse.isOK()) {
            return versionResponse.getStatus();
        }

        if (!versionResponse.getValue().commandStatus.isOK()) {
            if (versionResponse.getStatus().code() == ErrorCodes::CommandNotFound) {
                return {ErrorCodes::OperationFailed,
                        "featureCompatibilityVersion for cluster is 3.4, cannot add a shard with "
                        "version below 3.4. See "
                        "http://dochub.mongodb.org/core/3.4-feature-compatibility."};
            }
            return versionResponse.getValue().commandStatus;
        }
    }

    if (!MONGO_FAIL_POINT(dontUpsertShardIdentityOnNewShards)) {
        auto commandRequest = createShardIdentityUpsertForAddShard(txn, shardType.getName());

        LOG(2) << "going to insert shardIdentity document into shard: " << shardType;

        auto swCommandResponse =
            _runCommandForAddShard(txn, targeter.get(), "admin", commandRequest);
        if (!swCommandResponse.isOK()) {
            return swCommandResponse.getStatus();
        }

        auto commandResponse = std::move(swCommandResponse.getValue());

        BatchedCommandResponse batchResponse;
        auto batchResponseStatus =
            Shard::CommandResponse::processBatchWriteResponse(commandResponse, &batchResponse);
        if (!batchResponseStatus.isOK()) {
            return batchResponseStatus;
        }
    }

    log() << "going to insert new entry for shard into config.shards: " << shardType.toString();

    Status result = Grid::get(txn)->catalogClient(txn)->insertConfigDocument(
        txn, ShardType::ConfigNS, shardType.toBSON(), ShardingCatalogClient::kMajorityWriteConcern);
    if (!result.isOK()) {
        log() << "error adding shard: " << shardType.toBSON() << " err: " << result.reason();
        return result;
    }

    // Add all databases which were discovered on the new shard
    for (const auto& dbName : dbNamesStatus.getValue()) {
        DatabaseType dbt;
        dbt.setName(dbName);
        dbt.setPrimary(shardType.getName());
        dbt.setSharded(false);

        Status status = Grid::get(txn)->catalogClient(txn)->updateDatabase(txn, dbName, dbt);
        if (!status.isOK()) {
            log() << "adding shard " << shardConnectionString.toString()
                  << " even though could not add database " << dbName;
        }
    }

    // Record in changelog
    BSONObjBuilder shardDetails;
    shardDetails.append("name", shardType.getName());
    shardDetails.append("host", shardConnectionString.toString());

    Grid::get(txn)->catalogClient(txn)->logChange(
        txn, "addShard", "", shardDetails.obj(), ShardingCatalogClient::kMajorityWriteConcern);

    // Ensure the added shard is visible to this process.
    auto shardRegistry = Grid::get(txn)->shardRegistry();
    if (!shardRegistry->getShard(txn, shardType.getName()).isOK()) {
        return {ErrorCodes::OperationFailed,
                "Could not find shard metadata for shard after adding it. This most likely "
                "indicates that the shard was removed immediately after it was added."};
    }
    stopMonitoringGuard.Dismiss();

    return shardType.getName();
}

void ShardingCatalogManagerImpl::appendConnectionStats(executor::ConnectionPoolStats* stats) {
    _executorForAddShard->appendConnectionStats(stats);
}

Status ShardingCatalogManagerImpl::initializeShardingAwarenessOnUnawareShards(
    OperationContext* txn) {
    auto swShards = _getAllShardingUnawareShards(txn);
    if (!swShards.isOK()) {
        return swShards.getStatus();
    } else {
        auto shards = std::move(swShards.getValue());
        for (const auto& shard : shards) {
            auto status = upsertShardIdentityOnShard(txn, shard);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    // Note: this OK status means only that tasks to initialize sharding awareness on the shards
    // were scheduled against the task executor, not that the tasks actually succeeded.
    return Status::OK();
}

StatusWith<std::vector<ShardType>> ShardingCatalogManagerImpl::_getAllShardingUnawareShards(
    OperationContext* txn) {
    std::vector<ShardType> shards;
    auto findStatus = Grid::get(txn)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        txn,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString(ShardType::ConfigNS),
        BSON(
            "state" << BSON("$ne" << static_cast<std::underlying_type<ShardType::ShardState>::type>(
                                ShardType::ShardState::kShardAware))),  // shard is sharding unaware
        BSONObj(),                                                      // no sort
        boost::none);                                                   // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& doc : findStatus.getValue().docs) {
        auto shardRes = ShardType::fromBSON(doc);
        if (!shardRes.isOK()) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "Failed to parse shard " << causedBy(shardRes.getStatus())
                                  << doc};
        }

        Status validateStatus = shardRes.getValue().validate();
        if (!validateStatus.isOK()) {
            return {validateStatus.code(),
                    str::stream() << "Failed to validate shard " << causedBy(validateStatus)
                                  << doc};
        }

        shards.push_back(shardRes.getValue());
    }

    return shards;
}

Status ShardingCatalogManagerImpl::upsertShardIdentityOnShard(OperationContext* txn,
                                                              ShardType shardType) {

    auto commandRequest = createShardIdentityUpsertForAddShard(txn, shardType.getName());

    auto swConnString = ConnectionString::parse(shardType.getHost());
    if (!swConnString.isOK()) {
        return swConnString.getStatus();
    }

    // TODO: Don't create a detached Shard object, create a detached RemoteCommandTargeter
    // instead.
    const std::shared_ptr<Shard> shard{
        Grid::get(txn)->shardRegistry()->createConnection(swConnString.getValue())};
    invariant(shard);
    auto targeter = shard->getTargeter();

    _scheduleAddShardTask(
        std::move(shardType), std::move(targeter), std::move(commandRequest), false);

    return Status::OK();
}

void ShardingCatalogManagerImpl::cancelAddShardTaskIfNeeded(const ShardId& shardId) {
    stdx::lock_guard<stdx::mutex> lk(_addShardHandlesMutex);
    if (_hasAddShardHandle_inlock(shardId)) {
        auto cbHandle = _getAddShardHandle_inlock(shardId);
        _executorForAddShard->cancel(cbHandle);
        // Untrack the handle here so that if this shard is re-added before the CallbackCanceled
        // status is delivered to the callback, a new addShard task for the shard will be
        // created.
        _untrackAddShardHandle_inlock(shardId);
    }
}

void ShardingCatalogManagerImpl::_scheduleAddShardTaskUnlessCanceled(
    const CallbackArgs& cbArgs,
    const ShardType shardType,
    std::shared_ptr<RemoteCommandTargeter> targeter,
    const BSONObj commandRequest) {
    if (cbArgs.status == ErrorCodes::CallbackCanceled) {
        return;
    }
    _scheduleAddShardTask(
        std::move(shardType), std::move(targeter), std::move(commandRequest), true);
}

void ShardingCatalogManagerImpl::_scheduleAddShardTask(
    const ShardType shardType,
    std::shared_ptr<RemoteCommandTargeter> targeter,
    const BSONObj commandRequest,
    const bool isRetry) {
    stdx::lock_guard<stdx::mutex> lk(_addShardHandlesMutex);

    if (isRetry) {
        // Untrack the handle from scheduleWorkAt, and schedule a new addShard task.
        _untrackAddShardHandle_inlock(shardType.getName());
    } else {
        // We should never be able to schedule an addShard task while one is running, because
        // there is a unique index on the _id field in config.shards.
        invariant(!_hasAddShardHandle_inlock(shardType.getName()));
    }

    // Schedule the shardIdentity upsert request to run immediately, and track the handle.

    auto swHost = targeter->findHostWithMaxWait(ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                Milliseconds(kDefaultFindHostMaxWaitTime));
    if (!swHost.isOK()) {
        // A 3.2 mongos must have previously successfully communicated with hosts in this shard,
        // so a failure to find a host here is probably transient, and it is safe to retry.
        warning() << "Failed to find host for shard " << shardType
                  << " when trying to upsert a shardIdentity document, "
                  << causedBy(swHost.getStatus());
        const Date_t now = _executorForAddShard->now();
        const Date_t when = now + getAddShardTaskRetryInterval();
        _trackAddShardHandle_inlock(
            shardType.getName(),
            _executorForAddShard->scheduleWorkAt(
                when,
                stdx::bind(&ShardingCatalogManagerImpl::_scheduleAddShardTaskUnlessCanceled,
                           this,
                           stdx::placeholders::_1,
                           shardType,
                           std::move(targeter),
                           std::move(commandRequest))));
        return;
    }

    executor::RemoteCommandRequest request(
        swHost.getValue(), "admin", commandRequest, rpc::makeEmptyMetadata(), nullptr, Seconds(30));

    const RemoteCommandCallbackFn callback =
        stdx::bind(&ShardingCatalogManagerImpl::_handleAddShardTaskResponse,
                   this,
                   stdx::placeholders::_1,
                   shardType,
                   std::move(targeter));

    if (isRetry) {
        log() << "Retrying upsert of shardIdentity document into shard " << shardType.getName();
    }
    _trackAddShardHandle_inlock(shardType.getName(),
                                _executorForAddShard->scheduleRemoteCommand(request, callback));
}

void ShardingCatalogManagerImpl::_handleAddShardTaskResponse(
    const RemoteCommandCallbackArgs& cbArgs,
    ShardType shardType,
    std::shared_ptr<RemoteCommandTargeter> targeter) {
    stdx::unique_lock<stdx::mutex> lk(_addShardHandlesMutex);

    // If the callback has been canceled (either due to shutdown or the shard being removed), we
    // do not need to reschedule the task or update config.shards.
    Status responseStatus = cbArgs.response.status;
    if (responseStatus == ErrorCodes::CallbackCanceled) {
        return;
    }

    // If the handle no longer exists, the shard must have been removed, but the callback must not
    // have been canceled until after the task had completed. In this case as well, we do not need
    // to reschedule the task or update config.shards.
    if (!_hasAddShardHandle_inlock(shardType.getName())) {
        return;
    }

    // Untrack the handle from scheduleRemoteCommand regardless of whether the command
    // succeeded. If it failed, we will track the handle for the rescheduled task before
    // releasing the mutex.
    _untrackAddShardHandle_inlock(shardType.getName());

    // Examine the response to determine if the upsert succeeded.

    bool rescheduleTask = false;

    auto swResponse = cbArgs.response;
    if (!swResponse.isOK()) {
        warning() << "Failed to upsert shardIdentity document during addShard into shard "
                  << shardType.getName() << "(" << shardType.getHost()
                  << "). The shardIdentity upsert will continue to be retried. "
                  << causedBy(swResponse.status);
        rescheduleTask = true;
    } else {
        // Create a CommandResponse object in order to use processBatchWriteResponse.
        BSONObj responseObj = swResponse.data.getOwned();
        BSONObj responseMetadata = swResponse.metadata.getOwned();
        Status commandStatus = getStatusFromCommandResult(responseObj);
        Status writeConcernStatus = getWriteConcernStatusFromCommandResult(responseObj);
        Shard::CommandResponse commandResponse(std::move(responseObj),
                                               std::move(responseMetadata),
                                               std::move(commandStatus),
                                               std::move(writeConcernStatus));

        BatchedCommandResponse batchResponse;
        auto batchResponseStatus =
            Shard::CommandResponse::processBatchWriteResponse(commandResponse, &batchResponse);
        if (!batchResponseStatus.isOK()) {
            if (batchResponseStatus == ErrorCodes::DuplicateKey) {
                warning()
                    << "Received duplicate key error when inserting the shardIdentity "
                       "document into "
                    << shardType.getName() << "(" << shardType.getHost()
                    << "). This means the shard has a shardIdentity document with a clusterId "
                       "that differs from this cluster's clusterId. It may still belong to "
                       "or not have been properly removed from another cluster. The "
                       "shardIdentity upsert will continue to be retried.";
            } else {
                warning() << "Failed to upsert shardIdentity document into shard "
                          << shardType.getName() << "(" << shardType.getHost()
                          << ") during addShard. The shardIdentity upsert will continue to be "
                             "retried. "
                          << causedBy(batchResponseStatus);
            }
            rescheduleTask = true;
        }
    }

    if (rescheduleTask) {
        // If the command did not succeed, schedule the upsert shardIdentity task again with a
        // delay.
        const Date_t now = _executorForAddShard->now();
        const Date_t when = now + getAddShardTaskRetryInterval();

        // Track the handle from scheduleWorkAt.
        _trackAddShardHandle_inlock(
            shardType.getName(),
            _executorForAddShard->scheduleWorkAt(
                when,
                stdx::bind(&ShardingCatalogManagerImpl::_scheduleAddShardTaskUnlessCanceled,
                           this,
                           stdx::placeholders::_1,
                           shardType,
                           std::move(targeter),
                           std::move(cbArgs.request.cmdObj))));
        return;
    }

    // If the command succeeded, update config.shards to mark the shard as shardAware.

    // Release the _addShardHandlesMutex before updating config.shards, since it involves disk
    // I/O.
    // At worst, a redundant addShard task will be scheduled by a new primary if the current
    // primary fails during that write.
    lk.unlock();

    // This thread is part of a thread pool owned by the addShard TaskExecutor. Threads in that
    // pool are not created with Client objects associated with them, so a Client is created and
    // attached here to do the local update. The Client is destroyed at the end of the scope,
    // leaving the thread state as it was before.
    Client::initThread(getThreadName());
    ON_BLOCK_EXIT([&] { Client::destroy(); });

    // Use the thread's Client to create an OperationContext to perform the local write to
    // config.shards. This OperationContext will automatically be destroyed when it goes out of
    // scope at the end of this code block.
    auto txnPtr = cc().makeOperationContext();

    // Use kNoWaitWriteConcern to prevent waiting in this callback, since we don't handle a
    // failed response anyway. If the write is rolled back, the new config primary will attempt to
    // initialize sharding awareness on this shard again, and this update to config.shards will
    // be automatically retried then. If it fails because the shard was removed through the normal
    // removeShard path (so the entry in config.shards was deleted), no new addShard task will
    // get scheduled on the next transition to primary.
    auto updateStatus =
        Grid::get(txnPtr.get())
            ->catalogClient(txnPtr.get())
            ->updateConfigDocument(
                txnPtr.get(),
                ShardType::ConfigNS,
                BSON(ShardType::name(shardType.getName())),
                BSON("$set" << BSON(
                         ShardType::state()
                         << static_cast<std::underlying_type<ShardType::ShardState>::type>(
                             ShardType::ShardState::kShardAware))),
                false,
                kNoWaitWriteConcern);

    if (!updateStatus.isOK()) {
        warning() << "Failed to mark shard " << shardType.getName() << "(" << shardType.getHost()
                  << ") as shardAware in config.shards. This will be retried the next time a "
                     "config server transitions to primary. "
                  << causedBy(updateStatus.getStatus());
    }
}

BSONObj ShardingCatalogManagerImpl::createShardIdentityUpsertForAddShard(
    OperationContext* txn, const std::string& shardName) {
    std::unique_ptr<BatchedUpdateDocument> updateDoc(new BatchedUpdateDocument());

    BSONObjBuilder query;
    query.append("_id", "shardIdentity");
    query.append(ShardIdentityType::shardName(), shardName);
    query.append(ShardIdentityType::clusterId(), ClusterIdentityLoader::get(txn)->getClusterId());
    updateDoc->setQuery(query.obj());

    BSONObjBuilder update;
    {
        BSONObjBuilder set(update.subobjStart("$set"));
        set.append(ShardIdentityType::configsvrConnString(),
                   Grid::get(txn)->shardRegistry()->getConfigServerConnectionString().toString());
    }
    updateDoc->setUpdateExpr(update.obj());
    updateDoc->setUpsert(true);

    std::unique_ptr<BatchedUpdateRequest> updateRequest(new BatchedUpdateRequest());
    updateRequest->addToUpdates(updateDoc.release());

    BatchedCommandRequest commandRequest(updateRequest.release());
    commandRequest.setNS(NamespaceString::kConfigCollectionNamespace);
    commandRequest.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    return commandRequest.toBSON();
}

bool ShardingCatalogManagerImpl::_hasAddShardHandle_inlock(const ShardId& shardId) {
    return _addShardHandles.find(shardId) != _addShardHandles.end();
}

const CallbackHandle& ShardingCatalogManagerImpl::_getAddShardHandle_inlock(
    const ShardId& shardId) {
    invariant(_hasAddShardHandle_inlock(shardId));
    return _addShardHandles.find(shardId)->second;
}

void ShardingCatalogManagerImpl::_trackAddShardHandle_inlock(
    const ShardId shardId, const StatusWith<CallbackHandle>& swHandle) {
    if (swHandle.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(40219, swHandle.getStatus());
    _addShardHandles.insert(std::pair<ShardId, CallbackHandle>(shardId, swHandle.getValue()));
}

void ShardingCatalogManagerImpl::_untrackAddShardHandle_inlock(const ShardId& shardId) {
    auto it = _addShardHandles.find(shardId);
    invariant(it != _addShardHandles.end());
    _addShardHandles.erase(shardId);
}

}  // namespace mongo
