
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

#pragma once

#include <string>

#include "mongo/s/client/shard.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

/*
 * Maintains the targeting and command execution logic for a single shard. Performs polling of
 * the shard (if replica set).
 */
class ShardRemote : public Shard {
    MONGO_DISALLOW_COPYING(ShardRemote);

public:
    /**
     * Instantiates a new shard connection management object for the specified shard.
     */
    ShardRemote(const ShardId& id,
                const ConnectionString& originalConnString,
                std::unique_ptr<RemoteCommandTargeter> targeter);

    ~ShardRemote();

    const ConnectionString getConnString() const override;

    const ConnectionString originalConnString() const override {
        return _originalConnString;
    }

    std::shared_ptr<RemoteCommandTargeter> getTargeter() const override {
        return _targeter;
    }

    void updateReplSetMonitor(const HostAndPort& remoteHost,
                              const Status& remoteCommandStatus) override;

    std::string toString() const override;

    bool isRetriableError(ErrorCodes::Error code, RetryPolicy options) final;

    Status createIndexOnConfig(OperationContext* opCtx,
                               const NamespaceString& ns,
                               const BSONObj& keys,
                               bool unique) override;

    void updateLastCommittedOpTime(LogicalTime lastCommittedOpTime) final;

    LogicalTime getLastCommittedOpTime() const final;

private:
    /**
     * Returns the metadata that should be used when running commands against this shard with
     * the given read preference.
     */
    BSONObj _appendMetadataForCommand(OperationContext* opCtx,
                                      const ReadPreferenceSetting& readPref);

    StatusWith<Shard::CommandResponse> _runCommand(OperationContext* opCtx,
                                                   const ReadPreferenceSetting& readPref,
                                                   const std::string& dbname,
                                                   Milliseconds maxTimeMSOverride,
                                                   const BSONObj& cmdObj) final;

    StatusWith<Shard::QueryResponse> _runExhaustiveCursorCommand(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const std::string& dbName,
        Milliseconds maxTimeMSOverride,
        const BSONObj& cmdObj) final;

    StatusWith<QueryResponse> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcernLevel,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit) final;

    /**
     * Protects _lastCommittedOpTime.
     */
    mutable stdx::mutex _lastCommittedOpTimeMutex;

    /**
    * Logical time representing the latest opTime timestamp known to be in this shard's majority
    * committed snapshot. Only the latest time is kept because lagged secondaries may return earlier
    * times.
    */
    LogicalTime _lastCommittedOpTime;

    /**
     * Connection string for the shard at the creation time.
     */
    const ConnectionString _originalConnString;

    /**
     * Targeter for obtaining hosts from which to read or to which to write.
     */
    const std::shared_ptr<RemoteCommandTargeter> _targeter;
};

}  // namespace mongo
