
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

#include "mongo/db/logical_time_metadata_hook.h"

#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/rpc/metadata/logical_time_metadata.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace rpc {

namespace {
const char kOperationTimeFieldName[] = "operationTime";
}
LogicalTimeMetadataHook::LogicalTimeMetadataHook(ServiceContext* service) : _service(service) {}

Status LogicalTimeMetadataHook::writeRequestMetadata(OperationContext* opCtx,
                                                     BSONObjBuilder* metadataBob) {
    auto validator = LogicalTimeValidator::get(_service);
    if (!validator || !LogicalClock::get(_service)->isEnabled()) {
        return Status::OK();
    }

    auto newTime = LogicalClock::get(_service)->getClusterTime();
    LogicalTimeMetadata metadata(validator->trySignLogicalTime(newTime));
    metadata.writeToMetadata(metadataBob);
    return Status::OK();
}

Status LogicalTimeMetadataHook::readReplyMetadata(OperationContext* opCtx,
                                                  StringData replySource,
                                                  const BSONObj& metadataObj) {
    auto parseStatus = LogicalTimeMetadata::readFromMetadata(metadataObj);
    if (!parseStatus.isOK()) {
        return parseStatus.getStatus();
    }

    auto& signedTime = parseStatus.getValue().getSignedTime();

    // LogicalTimeMetadata is default constructed if no cluster time metadata was sent, so a
    // default constructed SignedLogicalTime should be ignored.
    if (signedTime.getTime() == LogicalTime::kUninitialized ||
        !LogicalClock::get(_service)->isEnabled()) {
        return Status::OK();
    }

    if (opCtx) {
        auto timeTracker = OperationTimeTracker::get(opCtx);

        auto operationTime = metadataObj[kOperationTimeFieldName];
        if (!operationTime.eoo()) {
            invariant(operationTime.type() == BSONType::bsonTimestamp);
            timeTracker->updateOperationTime(LogicalTime(operationTime.timestamp()));
        }
    }
    return LogicalClock::get(_service)->advanceClusterTime(signedTime.getTime());
}

}  // namespace rpc
}  // namespace mongo
