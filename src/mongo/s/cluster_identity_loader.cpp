
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

#include "mongo/s/cluster_identity_loader.h"

#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

const auto getClusterIdentity = ServiceContext::declareDecoration<ClusterIdentityLoader>();

}  // namespace

ClusterIdentityLoader* ClusterIdentityLoader::get(ServiceContext* serviceContext) {
    return &getClusterIdentity(serviceContext);
}

ClusterIdentityLoader* ClusterIdentityLoader::get(OperationContext* operationContext) {
    return ClusterIdentityLoader::get(operationContext->getServiceContext());
}

OID ClusterIdentityLoader::getClusterId() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_initializationState == InitializationState::kInitialized && _lastLoadResult.isOK());
    return _lastLoadResult.getValue();
}

Status ClusterIdentityLoader::loadClusterId(OperationContext* opCtx,
                                            const repl::ReadConcernLevel& readConcernLevel) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_initializationState == InitializationState::kInitialized) {
        invariant(_lastLoadResult.isOK());
        return Status::OK();
    }

    if (_initializationState == InitializationState::kLoading) {
        while (_initializationState == InitializationState::kLoading) {
            _inReloadCV.wait(lk);
        }
        return _lastLoadResult.getStatus();
    }

    invariant(_initializationState == InitializationState::kUninitialized);
    _initializationState = InitializationState::kLoading;

    lk.unlock();
    auto loadStatus = _fetchClusterIdFromConfig(opCtx, readConcernLevel);
    lk.lock();

    invariant(_initializationState == InitializationState::kLoading);
    _lastLoadResult = std::move(loadStatus);
    if (_lastLoadResult.isOK()) {
        _initializationState = InitializationState::kInitialized;
    } else {
        _initializationState = InitializationState::kUninitialized;
    }
    _inReloadCV.notify_all();
    return _lastLoadResult.getStatus();
}

StatusWith<OID> ClusterIdentityLoader::_fetchClusterIdFromConfig(
    OperationContext* opCtx, const repl::ReadConcernLevel& readConcernLevel) {
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto loadResult = catalogClient->getConfigVersion(opCtx, readConcernLevel);
    if (!loadResult.isOK()) {
        return loadResult.getStatus().withContext("Error loading clusterID");
    }
    return loadResult.getValue().getClusterId();
}

void ClusterIdentityLoader::discardCachedClusterId() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_initializationState == InitializationState::kUninitialized) {
        return;
    }
    invariant(_initializationState == InitializationState::kInitialized);
    _lastLoadResult = {
        Status{ErrorCodes::InternalError, "cluster ID never re-loaded after rollback"}};
    _initializationState = InitializationState::kUninitialized;
}

}  // namespace mongo
