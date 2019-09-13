
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

#include "mongo/db/server_recovery.h"

#include "mongo/db/namespace_string.h"

namespace mongo {
namespace {
const auto getInReplicationRecovery = ServiceContext::declareDecoration<bool>();
const auto getSizeRecoveryState = ServiceContext::declareDecoration<SizeRecoveryState>();
}  // namespace

bool SizeRecoveryState::collectionNeedsSizeAdjustment(const std::string& ident) const {
    if (!inReplicationRecovery(getGlobalServiceContext())) {
        return true;
    }

    return collectionAlwaysNeedsSizeAdjustment(ident);
}

bool SizeRecoveryState::collectionAlwaysNeedsSizeAdjustment(const std::string& ident) const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _collectionsAlwaysNeedingSizeAdjustment.count(ident) > 0;
}

void SizeRecoveryState::markCollectionAsAlwaysNeedsSizeAdjustment(const std::string& ident) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _collectionsAlwaysNeedingSizeAdjustment.insert(ident);
}

void SizeRecoveryState::clearStateBeforeRecovery() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _collectionsAlwaysNeedingSizeAdjustment.clear();
}
}  // namespace mongo

bool& mongo::inReplicationRecovery(ServiceContext* serviceCtx) {
    return getInReplicationRecovery(serviceCtx);
}

mongo::SizeRecoveryState& mongo::sizeRecoveryState(ServiceContext* serviceCtx) {
    return getSizeRecoveryState(serviceCtx);
}
