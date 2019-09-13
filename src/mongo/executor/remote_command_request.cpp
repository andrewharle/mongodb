
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

#include "mongo/executor/remote_command_request.h"

#include <ostream>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace executor {
namespace {

// Used to generate unique identifiers for requests so they can be traced throughout the
// asynchronous networking logs
AtomicUInt64 requestIdCounter(0);

}  // namespace

constexpr Milliseconds RemoteCommandRequest::kNoTimeout;
constexpr Date_t RemoteCommandRequest::kNoExpirationDate;

RemoteCommandRequest::RemoteCommandRequest() : id(requestIdCounter.addAndFetch(1)) {}

RemoteCommandRequest::RemoteCommandRequest(RequestId requestId,
                                           const HostAndPort& theTarget,
                                           const std::string& theDbName,
                                           const BSONObj& theCmdObj,
                                           const BSONObj& metadataObj,
                                           OperationContext* opCtx,
                                           Milliseconds timeoutMillis)
    : id(requestId),
      target(theTarget),
      dbname(theDbName),
      metadata(metadataObj),
      cmdObj(theCmdObj),
      opCtx(opCtx),
      timeout(timeoutMillis) {}

RemoteCommandRequest::RemoteCommandRequest(const HostAndPort& theTarget,
                                           const std::string& theDbName,
                                           const BSONObj& theCmdObj,
                                           const BSONObj& metadataObj,
                                           OperationContext* opCtx,
                                           Milliseconds timeoutMillis)
    : RemoteCommandRequest(requestIdCounter.addAndFetch(1),
                           theTarget,
                           theDbName,
                           theCmdObj,
                           metadataObj,
                           opCtx,
                           timeoutMillis) {}

std::string RemoteCommandRequest::toString() const {
    str::stream out;
    out << "RemoteCommand " << id << " -- target:" << target.toString() << " db:" << dbname;

    if (expirationDate != kNoExpirationDate) {
        out << " expDate:" << expirationDate.toString();
    }

    out << " cmd:" << cmdObj.toString();
    return out;
}

bool RemoteCommandRequest::operator==(const RemoteCommandRequest& rhs) const {
    if (this == &rhs) {
        return true;
    }
    return target == rhs.target && dbname == rhs.dbname &&
        SimpleBSONObjComparator::kInstance.evaluate(cmdObj == rhs.cmdObj) &&
        SimpleBSONObjComparator::kInstance.evaluate(metadata == rhs.metadata) &&
        timeout == rhs.timeout;
}

bool RemoteCommandRequest::operator!=(const RemoteCommandRequest& rhs) const {
    return !(*this == rhs);
}

std::ostream& operator<<(std::ostream& os, const RemoteCommandRequest& request) {
    return os << request.toString();
}

}  // namespace executor
}  // namespace mongo
