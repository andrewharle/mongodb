
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

#include <memory>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_builder_interface.h"

namespace mongo {
namespace rpc {

/**
 * Constructs an OP_COMMANDREPLY message.
 */
class CommandReplyBuilder : public ReplyBuilderInterface {
public:
    /**
     * Constructs an OP_COMMANDREPLY in a new buffer.
     */
    CommandReplyBuilder();

    /*
     * Constructs an OP_COMMANDREPLY in an existing buffer. Ownership of the buffer
     * will be transfered to the CommandReplyBuilder.
     */
    CommandReplyBuilder(Message&& message);


    CommandReplyBuilder& setRawCommandReply(const BSONObj& commandReply) final;
    BSONObjBuilder getInPlaceReplyBuilder(std::size_t) final;

    CommandReplyBuilder& setMetadata(const BSONObj& metadata) final;

    Protocol getProtocol() const final;

    void reset() final;

    /**
     * Writes data then transfers ownership of the message to the caller.
     * The behavior of calling any methods on the object is subsequently
     * undefined.
     */
    Message done() final;

private:
    enum class State { kMetadata, kCommandReply, kOutputDocs, kDone };

    // Default values are all empty.
    BufBuilder _builder{};
    Message _message;
    State _state{State::kCommandReply};
};

}  // namespace rpc
}  // namespace mongo
