/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <cstdint>
#include <memory>

#include "mongo/db/cursor_id.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Message;

template <typename T>
class StatusWith;

class StringData;
class NamespaceString;

namespace executor {
struct RemoteCommandRequest;
struct RemoteCommandResponse;

/**
 * Downconverts a find command request to the legacy (non-command) OP_QUERY format. The returned
 * message is formed, with the exception of the messageId header field, which must be set by
 * the caller before sending the message over the wire. Note that our legacy socket code sets the
 * messageId in MessagingPort::say().
 */
StatusWith<Message> downconvertFindCommandRequest(const RemoteCommandRequest& request);

/**
 * We may not be able to fit the entire batch from the OP_REPLY into a single response BSONObj, so
 * we must defer the parsing to the original requester. This method creates a RemoteCommandResponse
 * with a response that will signal callers to decode the raw message. The RemoteCommandResponse
 * returned by this method takes ownership of 'message'.
 */
StatusWith<RemoteCommandResponse> prepareOpReplyErrorResponse(std::uint32_t requestId,
                                                              StringData cursorNamespace,
                                                              Message* response);

/**
 * Downconverts a getMore command request to the legacy OP_GET_MORE format. The returned message
 * is fully formed, with the exception of the messageId header field, which must be set by the
 * the caller before sending the message over the wire. Note that our legacy socket code sets the
 * messageId in MessagingPort::say().
 */
StatusWith<Message> downconvertGetMoreCommandRequest(const RemoteCommandRequest& request);

}  // namespace mongo
}  // namespace executor
