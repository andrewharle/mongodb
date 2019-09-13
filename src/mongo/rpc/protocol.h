
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

#include <cstdint>
#include <string>
#include <type_traits>

#include "mongo/base/status_with.h"
#include "mongo/db/wire_version.h"
#include "mongo/rpc/message.h"

namespace mongo {
class BSONObj;
class OperationContext;
namespace rpc {

/**
 * Bit flags representing support for a particular RPC protocol.
 * This is just an internal representation, and is never transmitted over the wire. It should
 * never be used for any other feature detection in favor of max/min wire version.
 *
 * A new protocol must be added as the highest order bit flag so that it is prioritized in
 * negotiation.
 */
enum class Protocol : std::uint64_t {

    /**
     * The pre-3.2 OP_QUERY on db.$cmd protocol
     */
    kOpQuery = 1 << 0,

    /**
     * The 3.2-3.6 OP_COMMAND protocol.
     */
    kOpCommandV1 = 1 << 1,

    /**
     * The 3.6+ OP_MSG protocol.
     */
    kOpMsg = 1 << 2,
};

/**
 * Bitfield representing a set of supported RPC protocols.
 */
using ProtocolSet = std::underlying_type<Protocol>::type;

/**
 * This namespace contains predefined bitfields for common levels of protocol support.
 */
namespace supports {

const ProtocolSet kNone = ProtocolSet{0};
const ProtocolSet kOpQueryOnly = static_cast<ProtocolSet>(Protocol::kOpQuery);
const ProtocolSet kOpCommandOnly = static_cast<ProtocolSet>(Protocol::kOpCommandV1);
const ProtocolSet kOpMsgOnly = static_cast<ProtocolSet>(Protocol::kOpMsg);
const ProtocolSet kAll = kOpQueryOnly | kOpCommandOnly | kOpMsgOnly;

}  // namespace supports

Protocol protocolForMessage(const Message& message);

/**
 * Returns the protocol used to initiate the current operation.
 */
Protocol getOperationProtocol(OperationContext* opCtx);

/**
 * Sets the protocol used to initiate the current operation.
 */
void setOperationProtocol(OperationContext* opCtx, Protocol protocol);

/**
 * Returns the newest protocol supported by two parties.
 */
StatusWith<Protocol> negotiate(ProtocolSet fst, ProtocolSet snd);

/**
 * Converts a ProtocolSet to a string. Currently only the predefined ProtocolSets in the
 * 'supports' namespace are supported.
 *
 * This intentionally does not conform to the STL 'to_string' convention so that it will
 * not conflict with the to_string overload for uint64_t.
 */
StatusWith<StringData> toString(ProtocolSet protocols);

/**
 * Parses a ProtocolSet from a string. Currently only the predefined ProtocolSets in the
 * 'supports' namespace are supported
 */
StatusWith<ProtocolSet> parseProtocolSet(StringData repr);

/**
 * Validates client and server wire version. The server is returned from isMaster, and the client is
 * from WireSpec.instance().
 */
Status validateWireVersion(const WireVersionInfo client, const WireVersionInfo server);

/**
 * Struct to pass around information about protocol set and wire version.
 */
struct ProtocolSetAndWireVersionInfo {
    ProtocolSet protocolSet;
    WireVersionInfo version;
};

/**
 * Determines the ProtocolSet of a remote server from an isMaster reply.
 */
StatusWith<ProtocolSetAndWireVersionInfo> parseProtocolSetFromIsMasterReply(
    const BSONObj& isMasterReply);

/**
  * Computes supported protocols from wire versions.
  */
ProtocolSet computeProtocolSet(const WireVersionInfo version);

}  // namespace rpc
}  // namespace mongo
