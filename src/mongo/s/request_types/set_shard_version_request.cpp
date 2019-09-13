
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

#include "mongo/s/request_types/set_shard_version_request.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/query/query_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

const char kCmdName[] = "setShardVersion";
const char kConfigServer[] = "configdb";
const char kShardName[] = "shard";
const char kShardConnectionString[] = "shardHost";
const char kInit[] = "init";
const char kForceRefresh[] = "forceRefresh";
const char kAuthoritative[] = "authoritative";
const char kNoConnectionVersioning[] = "noConnectionVersioning";

}  // namespace

constexpr StringData SetShardVersionRequest::kVersion;

SetShardVersionRequest::SetShardVersionRequest(ConnectionString configServer,
                                               ShardId shardName,
                                               ConnectionString shardConnectionString)
    : _init(true),
      _isAuthoritative(true),
      _configServer(std::move(configServer)),
      _shardName(std::move(shardName)),
      _shardCS(std::move(shardConnectionString)) {}

SetShardVersionRequest::SetShardVersionRequest(ConnectionString configServer,
                                               ShardId shardName,
                                               ConnectionString shardConnectionString,
                                               NamespaceString nss,
                                               ChunkVersion version,
                                               bool isAuthoritative,
                                               bool forceRefresh)
    : _init(false),
      _isAuthoritative(isAuthoritative),
      _forceRefresh(forceRefresh),
      _configServer(std::move(configServer)),
      _shardName(std::move(shardName)),
      _shardCS(std::move(shardConnectionString)),
      _nss(std::move(nss)),
      _version(std::move(version)) {}

SetShardVersionRequest::SetShardVersionRequest() = default;

SetShardVersionRequest SetShardVersionRequest::makeForInit(
    const ConnectionString& configServer,
    const ShardId& shardName,
    const ConnectionString& shardConnectionString) {
    return SetShardVersionRequest(configServer, shardName, shardConnectionString);
}

SetShardVersionRequest SetShardVersionRequest::makeForVersioning(
    const ConnectionString& configServer,
    const ShardId& shardName,
    const ConnectionString& shardConnectionString,
    const NamespaceString& nss,
    const ChunkVersion& nssVersion,
    bool isAuthoritative,
    bool forceRefresh) {
    invariant(nss.isValid());
    return SetShardVersionRequest(configServer,
                                  shardName,
                                  shardConnectionString,
                                  nss,
                                  nssVersion,
                                  isAuthoritative,
                                  forceRefresh);
}

SetShardVersionRequest SetShardVersionRequest::makeForVersioningNoPersist(
    const ConnectionString& configServer,
    const ShardId& shardName,
    const ConnectionString& shard,
    const NamespaceString& nss,
    const ChunkVersion& nssVersion,
    bool isAuthoritative,
    bool forceRefresh) {
    auto ssv = makeForVersioning(
        configServer, shardName, shard, nss, nssVersion, isAuthoritative, forceRefresh);
    ssv._noConnectionVersioning = true;

    return ssv;
}

StatusWith<SetShardVersionRequest> SetShardVersionRequest::parseFromBSON(const BSONObj& cmdObj) {
    SetShardVersionRequest request;

    {
        std::string shardName;
        Status status = bsonExtractStringField(cmdObj, kShardName, &shardName);
        request._shardName = ShardId(shardName);

        if (!status.isOK())
            return status;
    }

    {
        std::string shardCS;
        Status status = bsonExtractStringField(cmdObj, kShardConnectionString, &shardCS);
        if (!status.isOK())
            return status;

        auto shardCSStatus = ConnectionString::parse(shardCS);
        if (!shardCSStatus.isOK())
            return shardCSStatus.getStatus();

        request._shardCS = std::move(shardCSStatus.getValue());
    }

    {
        Status status = bsonExtractBooleanFieldWithDefault(cmdObj, kInit, false, &request._init);
        if (!status.isOK())
            return status;
    }

    {
        Status status = bsonExtractBooleanFieldWithDefault(
            cmdObj, kForceRefresh, false, &request._forceRefresh);
        if (!status.isOK())
            return status;
    }

    {
        Status status = bsonExtractBooleanFieldWithDefault(
            cmdObj, kAuthoritative, false, &request._isAuthoritative);
        if (!status.isOK())
            return status;
    }

    {
        Status status = bsonExtractBooleanFieldWithDefault(
            cmdObj, kNoConnectionVersioning, false, &request._noConnectionVersioning);
        if (!status.isOK())
            return status;
    }

    if (request.isInit()) {
        return request;
    }

    // Only initialize the version information if this is not an "init" request

    {
        std::string ns;
        Status status = bsonExtractStringField(cmdObj, kCmdName, &ns);
        if (!status.isOK())
            return status;

        NamespaceString nss(ns);

        if (!nss.isValid()) {
            return {ErrorCodes::InvalidNamespace,
                    str::stream() << ns << " is not a valid namespace"};
        }

        request._nss = std::move(nss);
    }

    {
        auto versionStatus = ChunkVersion::parseLegacyWithField(cmdObj, kVersion);
        if (!versionStatus.isOK())
            return versionStatus.getStatus();

        request._version = versionStatus.getValue();
    }

    return request;
}

BSONObj SetShardVersionRequest::toBSON() const {
    BSONObjBuilder cmdBuilder;

    cmdBuilder.append(kCmdName, _init ? "" : _nss.get().ns());
    cmdBuilder.append(kInit, _init);
    cmdBuilder.append(kForceRefresh, _forceRefresh);
    cmdBuilder.append(kAuthoritative, _isAuthoritative);
    // 'configdb' field is only included for v3.4 backwards compatibility
    cmdBuilder.append(kConfigServer, _configServer.toString());
    cmdBuilder.append(kShardName, _shardName.toString());
    cmdBuilder.append(kShardConnectionString, _shardCS.toString());

    if (_init) {
        // Always include a 30 second timeout on sharding state initialization, to work around
        // SERVER-21458.
        cmdBuilder.append(QueryRequest::cmdOptionMaxTimeMS, 30000);
    } else {
        _version->appendLegacyWithField(&cmdBuilder, kVersion);
    }

    if (_noConnectionVersioning) {
        cmdBuilder.append(kNoConnectionVersioning, true);
    }

    return cmdBuilder.obj();
}

const NamespaceString& SetShardVersionRequest::getNS() const {
    invariant(!_init);
    return _nss.get();
}

const ChunkVersion SetShardVersionRequest::getNSVersion() const {
    invariant(!_init);
    return _version.get();
}

}  // namespace mongo
