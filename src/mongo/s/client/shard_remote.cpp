
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard_remote.h"

#include <algorithm>
#include <string>

#include "mongo/client/fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/server_parameters.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::string;

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using rpc::TrackingMetadata;
using RemoteCommandCallbackArgs = TaskExecutor::RemoteCommandCallbackArgs;

namespace {
// Include kReplSetMetadataFieldName in a request to get the shard's ReplSetMetadata in the
// response.
const BSONObj kReplMetadata(BSON(rpc::kReplSetMetadataFieldName << 1));

constexpr bool internalProhibitShardOperationRetryByDefault = false;
MONGO_EXPORT_SERVER_PARAMETER(internalProhibitShardOperationRetry,
                              bool,
                              internalProhibitShardOperationRetryByDefault);

/**
 * Returns a new BSONObj describing the same command and arguments as 'cmdObj', but with maxTimeMS
 * replaced by maxTimeMSOverride (or removed if maxTimeMSOverride is Milliseconds::max()).
 */
BSONObj appendMaxTimeToCmdObj(Milliseconds maxTimeMSOverride, const BSONObj& cmdObj) {
    BSONObjBuilder updatedCmdBuilder;

    // Remove the user provided maxTimeMS so we can attach the one from the override
    for (const auto& elem : cmdObj) {
        if (!str::equals(elem.fieldName(), QueryRequest::cmdOptionMaxTimeMS)) {
            updatedCmdBuilder.append(elem);
        }
    }

    if (maxTimeMSOverride < Milliseconds::max()) {
        updatedCmdBuilder.append(QueryRequest::cmdOptionMaxTimeMS,
                                 durationCount<Milliseconds>(maxTimeMSOverride));
    }

    return updatedCmdBuilder.obj();
}

}  // unnamed namespace

ShardRemote::ShardRemote(const ShardId& id,
                         const ConnectionString& originalConnString,
                         std::unique_ptr<RemoteCommandTargeter> targeter)
    : Shard(id), _originalConnString(originalConnString), _targeter(targeter.release()) {}

ShardRemote::~ShardRemote() = default;

bool ShardRemote::isRetriableError(ErrorCodes::Error code, RetryPolicy options) {
    if (internalProhibitShardOperationRetry.loadRelaxed()) {
        return false;
    }

    if (options == RetryPolicy::kNoRetry) {
        return false;
    }

    const auto& retriableErrors = options == RetryPolicy::kIdempotent
        ? RemoteCommandRetryScheduler::kAllRetriableErrors
        : RemoteCommandRetryScheduler::kNotMasterErrors;
    return std::find(retriableErrors.begin(), retriableErrors.end(), code) != retriableErrors.end();
}

const ConnectionString ShardRemote::getConnString() const {
    return _targeter->connectionString();
}

// Any error code changes should possibly also be made to Shard::shouldErrorBePropagated!
void ShardRemote::updateReplSetMonitor(const HostAndPort& remoteHost,
                                       const Status& remoteCommandStatus) {
    if (remoteCommandStatus.isOK())
        return;

    if (ErrorCodes::isNotMasterError(remoteCommandStatus.code())) {
        _targeter->markHostNotMaster(remoteHost, remoteCommandStatus);
    } else if (ErrorCodes::isNetworkError(remoteCommandStatus.code())) {
        _targeter->markHostUnreachable(remoteHost, remoteCommandStatus);
    } else if (remoteCommandStatus == ErrorCodes::NetworkInterfaceExceededTimeLimit) {
        _targeter->markHostUnreachable(remoteHost, remoteCommandStatus);
    }
}

void ShardRemote::updateLastCommittedOpTime(LogicalTime lastCommittedOpTime) {
    stdx::lock_guard<stdx::mutex> lk(_lastCommittedOpTimeMutex);

    // A secondary may return a lastCommittedOpTime less than the latest seen so far.
    if (lastCommittedOpTime > _lastCommittedOpTime) {
        _lastCommittedOpTime = lastCommittedOpTime;
    }
}

LogicalTime ShardRemote::getLastCommittedOpTime() const {
    stdx::lock_guard<stdx::mutex> lk(_lastCommittedOpTimeMutex);
    return _lastCommittedOpTime;
}

std::string ShardRemote::toString() const {
    return getId().toString() + ":" + _originalConnString.toString();
}

BSONObj ShardRemote::_appendMetadataForCommand(OperationContext* opCtx,
                                               const ReadPreferenceSetting& readPref) {
    BSONObjBuilder builder;
    if (logger::globalLogDomain()->shouldLog(
            logger::LogComponent::kTracking,
            logger::LogSeverity::Debug(1))) {  // avoid performance overhead if not logging
        if (!TrackingMetadata::get(opCtx).getIsLogged()) {
            if (!TrackingMetadata::get(opCtx).getOperId()) {
                TrackingMetadata::get(opCtx).initWithOperName("NotSet");
            }
            MONGO_LOG_COMPONENT(1, logger::LogComponent::kTracking)
                << TrackingMetadata::get(opCtx).toString();
            TrackingMetadata::get(opCtx).setIsLogged(true);
        }

        TrackingMetadata metadata = TrackingMetadata::get(opCtx).constructChildMetadata();
        metadata.writeToMetadata(&builder);
    }

    readPref.toContainingBSON(&builder);

    if (isConfig())
        builder.appendElements(kReplMetadata);

    return builder.obj();
}

StatusWith<Shard::CommandResponse> ShardRemote::_runCommand(OperationContext* opCtx,
                                                            const ReadPreferenceSetting& readPref,
                                                            const string& dbName,
                                                            Milliseconds maxTimeMSOverride,
                                                            const BSONObj& cmdObj) {

    ReadPreferenceSetting readPrefWithMinOpTime(readPref);
    if (isConfig()) {
        readPrefWithMinOpTime.minOpTime = Grid::get(opCtx)->configOpTime();
    }
    const auto swHost = _targeter->findHost(opCtx, readPrefWithMinOpTime);
    if (!swHost.isOK()) {
        return swHost.getStatus();
    }
    const auto host = std::move(swHost.getValue());

    const Milliseconds requestTimeout =
        std::min(opCtx->getRemainingMaxTimeMillis(), maxTimeMSOverride);

    const RemoteCommandRequest request(
        host,
        dbName,
        appendMaxTimeToCmdObj(requestTimeout, cmdObj),
        _appendMetadataForCommand(opCtx, readPrefWithMinOpTime),
        opCtx,
        requestTimeout < Milliseconds::max() ? requestTimeout : RemoteCommandRequest::kNoTimeout);

    RemoteCommandResponse response =
        Status(ErrorCodes::InternalError,
               str::stream() << "Failed to run remote command request " << request.toString());

    TaskExecutor* executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto swCallbackHandle = executor->scheduleRemoteCommand(
        request, [&response](const RemoteCommandCallbackArgs& args) { response = args.response; });
    if (!swCallbackHandle.isOK()) {
        return swCallbackHandle.getStatus();
    }

    // Block until the command is carried out
    executor->wait(swCallbackHandle.getValue());

    updateReplSetMonitor(host, response.status);

    if (!response.status.isOK()) {
        if (ErrorCodes::isExceededTimeLimitError(response.status.code())) {
            LOG(0) << "Operation timed out with status " << redact(response.status);
        }
        return response.status;
    }

    auto result = response.data.getOwned();
    auto commandStatus = getStatusFromCommandResult(result);
    auto writeConcernStatus = getWriteConcernStatusFromCommandResult(result);

    updateReplSetMonitor(host, commandStatus);
    updateReplSetMonitor(host, writeConcernStatus);

    return Shard::CommandResponse(std::move(host),
                                  std::move(result),
                                  response.metadata.getOwned(),
                                  std::move(commandStatus),
                                  std::move(writeConcernStatus));
}

StatusWith<Shard::QueryResponse> ShardRemote::_runExhaustiveCursorCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const string& dbName,
    Milliseconds maxTimeMSOverride,
    const BSONObj& cmdObj) {
    const auto host = _targeter->findHost(opCtx, readPref);
    if (!host.isOK()) {
        return host.getStatus();
    }

    QueryResponse response;

    // If for some reason the callback never gets invoked, we will return this status in response.
    Status status =
        Status(ErrorCodes::InternalError, "Internal error running cursor callback in command");

    auto fetcherCallback = [&status, &response](const Fetcher::QueryResponseStatus& dataStatus,
                                                Fetcher::NextAction* nextAction,
                                                BSONObjBuilder* getMoreBob) {

        // Throw out any accumulated results on error
        if (!dataStatus.isOK()) {
            status = dataStatus.getStatus();
            response.docs.clear();
            return;
        }

        const auto& data = dataStatus.getValue();

        if (data.otherFields.metadata.hasField(rpc::kReplSetMetadataFieldName)) {
            auto replParseStatus =
                rpc::ReplSetMetadata::readFromMetadata(data.otherFields.metadata);
            if (!replParseStatus.isOK()) {
                status = replParseStatus.getStatus();
                response.docs.clear();
                return;
            }

            const auto& replSetMetadata = replParseStatus.getValue();
            response.opTime = replSetMetadata.getLastOpCommitted();
        }

        for (const BSONObj& doc : data.documents) {
            response.docs.push_back(doc.getOwned());
        }

        status = Status::OK();

        if (!getMoreBob) {
            return;
        }
        getMoreBob->append("getMore", data.cursorId);
        getMoreBob->append("collection", data.nss.coll());
    };

    const Milliseconds requestTimeout =
        std::min(opCtx->getRemainingMaxTimeMillis(), maxTimeMSOverride);

    Fetcher fetcher(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                    host.getValue(),
                    dbName,
                    cmdObj,
                    fetcherCallback,
                    _appendMetadataForCommand(opCtx, readPref),
                    requestTimeout, /* command network timeout */
                    requestTimeout /* getMore network timeout */);

    Status scheduleStatus = fetcher.schedule();
    if (!scheduleStatus.isOK()) {
        return scheduleStatus;
    }

    fetcher.join();

    updateReplSetMonitor(host.getValue(), status);

    if (!status.isOK()) {
        if (ErrorCodes::isExceededTimeLimitError(status.code())) {
            LOG(0) << "Operation timed out " << causedBy(status);
        }
        return status;
    }

    return response;
}


StatusWith<Shard::QueryResponse> ShardRemote::_exhaustiveFindOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    invariant(isConfig());
    auto const grid = Grid::get(opCtx);

    ReadPreferenceSetting readPrefWithMinOpTime(readPref);
    readPrefWithMinOpTime.minOpTime = grid->configOpTime();

    BSONObj readConcernObj;
    {
        invariant(readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern);
        const repl::ReadConcernArgs readConcern(grid->configOpTime(), readConcernLevel);
        BSONObjBuilder bob;
        readConcern.appendInfo(&bob);
        readConcernObj =
            bob.done().getObjectField(repl::ReadConcernArgs::kReadConcernFieldName).getOwned();
    }

    const Milliseconds maxTimeMS =
        std::min(opCtx->getRemainingMaxTimeMillis(), kDefaultConfigCommandTimeout);

    BSONObjBuilder findCmdBuilder;

    {
        QueryRequest qr(nss);
        qr.setFilter(query);
        qr.setSort(sort);
        qr.setReadConcern(readConcernObj);
        qr.setLimit(limit);

        if (maxTimeMS < Milliseconds::max()) {
            qr.setMaxTimeMS(durationCount<Milliseconds>(maxTimeMS));
        }

        qr.asFindCommand(&findCmdBuilder);
    }

    return _runExhaustiveCursorCommand(
        opCtx, readPrefWithMinOpTime, nss.db().toString(), maxTimeMS, findCmdBuilder.done());
}

Status ShardRemote::createIndexOnConfig(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const BSONObj& keys,
                                        bool unique) {
    MONGO_UNREACHABLE;
}

}  // namespace mongo
