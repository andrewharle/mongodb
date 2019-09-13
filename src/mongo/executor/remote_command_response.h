
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
#include <iosfwd>
#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/message.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace rpc {
class ReplyInterface;
}  // namespace rpc

namespace executor {


/**
 * Type of object describing the response of previously sent RemoteCommandRequest.
 */
struct RemoteCommandResponse {
    RemoteCommandResponse() = default;

    RemoteCommandResponse(ErrorCodes::Error code, std::string reason);

    RemoteCommandResponse(ErrorCodes::Error code, std::string reason, Milliseconds millis);

    RemoteCommandResponse(Status s);

    RemoteCommandResponse(Status s, Milliseconds millis);

    RemoteCommandResponse(BSONObj dataObj, BSONObj metadataObj, Milliseconds millis);

    RemoteCommandResponse(Message messageArg,
                          BSONObj dataObj,
                          BSONObj metadataObj,
                          Milliseconds millis);

    RemoteCommandResponse(const rpc::ReplyInterface& rpcReply, Milliseconds millis);

    bool isOK() const;

    std::string toString() const;

    bool operator==(const RemoteCommandResponse& rhs) const;
    bool operator!=(const RemoteCommandResponse& rhs) const;

    std::shared_ptr<const Message> message;  // May be null.
    BSONObj data;                            // Always owned. May point into message.
    BSONObj metadata;                        // Always owned. May point into message.
    boost::optional<Milliseconds> elapsedMillis;
    Status status = Status::OK();
};

std::ostream& operator<<(std::ostream& os, const RemoteCommandResponse& request);

}  // namespace executor
}  // namespace mongo
