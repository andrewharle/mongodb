
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

#include <boost/optional.hpp>
#include <vector>

#include "mongo/bson/oid.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/dist_lock_catalog.h"
#include "mongo/util/time_support.h"

namespace mongo {

class FindAndModifyRequest;
struct ReadPreferenceSetting;

class DistLockCatalogImpl final : public DistLockCatalog {
public:
    DistLockCatalogImpl();
    ~DistLockCatalogImpl();

    StatusWith<LockpingsType> getPing(OperationContext* opCtx, StringData processID) override;

    Status ping(OperationContext* opCtx, StringData processID, Date_t ping) override;

    StatusWith<LocksType> grabLock(OperationContext* opCtx,
                                   StringData lockID,
                                   const OID& lockSessionID,
                                   StringData who,
                                   StringData processId,
                                   Date_t time,
                                   StringData why,
                                   const WriteConcernOptions& writeConcern) override;

    StatusWith<LocksType> overtakeLock(OperationContext* opCtx,
                                       StringData lockID,
                                       const OID& lockSessionID,
                                       const OID& currentHolderTS,
                                       StringData who,
                                       StringData processId,
                                       Date_t time,
                                       StringData why) override;

    Status unlock(OperationContext* opCtx, const OID& lockSessionID) override;

    Status unlock(OperationContext* opCtx, const OID& lockSessionID, StringData name) override;

    Status unlockAll(OperationContext* opCtx, const std::string& processID) override;

    StatusWith<ServerInfo> getServerInfo(OperationContext* opCtx) override;

    StatusWith<LocksType> getLockByTS(OperationContext* opCtx, const OID& lockSessionID) override;

    StatusWith<LocksType> getLockByName(OperationContext* opCtx, StringData name) override;

    Status stopPing(OperationContext* opCtx, StringData processId) override;

private:
    Status _unlock(OperationContext* opCtx, const FindAndModifyRequest& request);

    StatusWith<std::vector<BSONObj>> _findOnConfig(OperationContext* opCtx,
                                                   const ReadPreferenceSetting& readPref,
                                                   const NamespaceString& nss,
                                                   const BSONObj& query,
                                                   const BSONObj& sort,
                                                   boost::optional<long long> limit);

    // These are not static to avoid initialization order fiasco.
    const NamespaceString _lockPingNS;
    const NamespaceString _locksNS;
};

}  // namespace mongo
