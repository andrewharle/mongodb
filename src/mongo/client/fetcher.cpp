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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/client/fetcher.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
const char* kCursorFieldName = "cursor";
const char* kCursorIdFieldName = "id";
const char* kNamespaceFieldName = "ns";

const char* kFirstBatchFieldName = "firstBatch";
const char* kNextBatchFieldName = "nextBatch";

/**
 * Parses cursor response in command result for cursor ID, namespace and documents.
 * 'batchFieldName' will be 'firstBatch' for the initial remote command invocation and
 * 'nextBatch' for getMore.
 */
Status parseCursorResponse(const BSONObj& obj,
                           const std::string& batchFieldName,
                           Fetcher::QueryResponse* batchData) {
    invariant(obj.isOwned());
    invariant(batchFieldName == kFirstBatchFieldName || batchFieldName == kNextBatchFieldName);
    invariant(batchData);

    BSONElement cursorElement = obj.getField(kCursorFieldName);
    if (cursorElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain '" << kCursorFieldName
                                    << "' field: "
                                    << obj);
    }
    if (!cursorElement.isABSONObj()) {
        return Status(
            ErrorCodes::FailedToParse,
            str::stream() << "'" << kCursorFieldName << "' field must be an object: " << obj);
    }
    BSONObj cursorObj = cursorElement.Obj();

    BSONElement cursorIdElement = cursorObj.getField(kCursorIdFieldName);
    if (cursorIdElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain '" << kCursorFieldName << "."
                                    << kCursorIdFieldName
                                    << "' field: "
                                    << obj);
    }
    if (cursorIdElement.type() != mongo::NumberLong) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "'" << kCursorFieldName << "." << kCursorIdFieldName
                                    << "' field must be a 'long' but was a '"
                                    << typeName(cursorIdElement.type())
                                    << "': "
                                    << obj);
    }
    batchData->cursorId = cursorIdElement.numberLong();

    BSONElement namespaceElement = cursorObj.getField(kNamespaceFieldName);
    if (namespaceElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain "
                                    << "'"
                                    << kCursorFieldName
                                    << "."
                                    << kNamespaceFieldName
                                    << "' field: "
                                    << obj);
    }
    if (namespaceElement.type() != mongo::String) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "'" << kCursorFieldName << "." << kNamespaceFieldName
                                    << "' field must be a string: "
                                    << obj);
    }
    NamespaceString tempNss(namespaceElement.valuestrsafe());
    if (!tempNss.isValid()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "'" << kCursorFieldName << "." << kNamespaceFieldName
                                    << "' contains an invalid namespace: "
                                    << obj);
    }
    batchData->nss = tempNss;

    BSONElement batchElement = cursorObj.getField(batchFieldName);
    if (batchElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain '" << kCursorFieldName << "."
                                    << batchFieldName
                                    << "' field: "
                                    << obj);
    }
    if (!batchElement.isABSONObj()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "'" << kCursorFieldName << "." << batchFieldName
                                    << "' field must be an array: "
                                    << obj);
    }
    BSONObj batchObj = batchElement.Obj();
    for (auto itemElement : batchObj) {
        if (!itemElement.isABSONObj()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "found non-object " << itemElement << " in "
                                        << "'"
                                        << kCursorFieldName
                                        << "."
                                        << batchFieldName
                                        << "' field: "
                                        << obj);
        }
        batchData->documents.push_back(itemElement.Obj());
    }

    for (auto& doc : batchData->documents) {
        doc.shareOwnershipWith(obj);
    }

    return Status::OK();
}

}  // namespace

Fetcher::Fetcher(executor::TaskExecutor* executor,
                 const HostAndPort& source,
                 const std::string& dbname,
                 const BSONObj& findCmdObj,
                 const CallbackFn& work,
                 const BSONObj& metadata,
                 Milliseconds timeout,
                 std::unique_ptr<RemoteCommandRetryScheduler::RetryPolicy> firstCommandRetryPolicy)
    : _executor(executor),
      _source(source),
      _dbname(dbname),
      _cmdObj(findCmdObj.getOwned()),
      _metadata(metadata.getOwned()),
      _work(work),
      _timeout(timeout),
      _firstRemoteCommandScheduler(
          _executor,
          RemoteCommandRequest(_source, _dbname, _cmdObj, _metadata, nullptr, _timeout),
          stdx::bind(&Fetcher::_callback, this, stdx::placeholders::_1, kFirstBatchFieldName),
          std::move(firstCommandRetryPolicy)) {
    uassert(ErrorCodes::BadValue, "callback function cannot be null", work);
}

Fetcher::~Fetcher() {
    DESTRUCTOR_GUARD(shutdown(); join(););
}

HostAndPort Fetcher::getSource() const {
    return _source;
}

BSONObj Fetcher::getCommandObject() const {
    return _cmdObj;
}

BSONObj Fetcher::getMetadataObject() const {
    return _metadata;
}

Milliseconds Fetcher::getTimeout() const {
    return _timeout;
}

std::string Fetcher::toString() const {
    return getDiagnosticString();
}

std::string Fetcher::getDiagnosticString() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    str::stream output;
    output << "Fetcher";
    output << " source: " << _source.toString();
    output << " database: " << _dbname;
    output << " query: " << _cmdObj;
    output << " query metadata: " << _metadata;
    output << " active: " << _active;
    output << " timeout: " << _timeout;
    output << " inShutdown: " << _inShutdown;
    output << " first: " << _first;
    output << " firstCommandScheduler: " << _firstRemoteCommandScheduler.toString();

    if (_getMoreCallbackHandle.isValid()) {
        output << " getMoreHandle.valid: " << _getMoreCallbackHandle.isValid();
        output << " getMoreHandle.cancelled: " << _getMoreCallbackHandle.isCanceled();
    }

    return output;
}

bool Fetcher::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _active;
}

Status Fetcher::schedule() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_active) {
        return Status(ErrorCodes::IllegalOperation, "fetcher already scheduled");
    }

    auto status = _firstRemoteCommandScheduler.startup();
    if (!status.isOK()) {
        return status;
    }

    _active = true;
    return Status::OK();
}

void Fetcher::shutdown() {
    executor::TaskExecutor::CallbackHandle handle;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;

        if (!_active) {
            return;
        }

        _firstRemoteCommandScheduler.shutdown();

        if (!_getMoreCallbackHandle.isValid()) {
            return;
        }

        handle = _getMoreCallbackHandle;
    }

    _executor->cancel(handle);
}

void Fetcher::join() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _condition.wait(lk, [this]() { return !_active; });
}

bool Fetcher::inShutdown_forTest() const {
    return _isInShutdown();
}

bool Fetcher::_isInShutdown() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _inShutdown;
}

Status Fetcher::_scheduleGetMore(const BSONObj& cmdObj) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_inShutdown) {
        return Status(ErrorCodes::CallbackCanceled,
                      "fetcher was shut down after previous batch was processed");
    }
    StatusWith<executor::TaskExecutor::CallbackHandle> scheduleResult =
        _executor->scheduleRemoteCommand(
            RemoteCommandRequest(_source, _dbname, cmdObj, _metadata, nullptr, _timeout),
            stdx::bind(&Fetcher::_callback, this, stdx::placeholders::_1, kNextBatchFieldName));

    if (!scheduleResult.isOK()) {
        return scheduleResult.getStatus();
    }

    _getMoreCallbackHandle = scheduleResult.getValue();

    return Status::OK();
}

void Fetcher::_callback(const RemoteCommandCallbackArgs& rcbd, const char* batchFieldName) {
    if (!rcbd.response.isOK()) {
        _work(StatusWith<Fetcher::QueryResponse>(rcbd.response.status), nullptr, nullptr);
        _finishCallback();
        return;
    }

    if (_isInShutdown()) {
        _work(Status(ErrorCodes::CallbackCanceled, "fetcher shutting down"), nullptr, nullptr);
        _finishCallback();
        return;
    }

    const BSONObj& queryResponseObj = rcbd.response.data;
    Status status = getStatusFromCommandResult(queryResponseObj);
    if (!status.isOK()) {
        _work(StatusWith<Fetcher::QueryResponse>(status), nullptr, nullptr);
        _finishCallback();
        return;
    }

    QueryResponse batchData;
    status = parseCursorResponse(queryResponseObj, batchFieldName, &batchData);
    if (!status.isOK()) {
        _work(StatusWith<Fetcher::QueryResponse>(status), nullptr, nullptr);
        _finishCallback();
        return;
    }

    batchData.otherFields.metadata = std::move(rcbd.response.metadata);
    batchData.elapsedMillis = rcbd.response.elapsedMillis.value_or(Milliseconds{0});
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        batchData.first = _first;
        _first = false;
    }

    NextAction nextAction = NextAction::kNoAction;

    if (!batchData.cursorId) {
        _work(StatusWith<QueryResponse>(batchData), &nextAction, nullptr);
        _finishCallback();
        return;
    }

    nextAction = NextAction::kGetMore;

    BSONObjBuilder bob;
    _work(StatusWith<QueryResponse>(batchData), &nextAction, &bob);

    // Callback function _work may modify nextAction to request the fetcher
    // not to schedule a getMore command.
    if (nextAction != NextAction::kGetMore) {
        _sendKillCursors(batchData.cursorId, batchData.nss);
        _finishCallback();
        return;
    }

    // Callback function may also disable the fetching of additional data by not filling in the
    // BSONObjBuilder for the getMore command.
    auto cmdObj = bob.obj();
    if (cmdObj.isEmpty()) {
        _sendKillCursors(batchData.cursorId, batchData.nss);
        _finishCallback();
        return;
    }

    status = _scheduleGetMore(cmdObj);
    if (!status.isOK()) {
        nextAction = NextAction::kNoAction;
        _work(StatusWith<Fetcher::QueryResponse>(status), nullptr, nullptr);
        _sendKillCursors(batchData.cursorId, batchData.nss);
        _finishCallback();
        return;
    }
}

void Fetcher::_sendKillCursors(const CursorId id, const NamespaceString& nss) {
    if (id) {
        auto logKillCursorsResult = [](const RemoteCommandCallbackArgs& args) {
            if (!args.response.isOK()) {
                warning() << "killCursors command task failed: " << redact(args.response.status);
                return;
            }
            auto status = getStatusFromCommandResult(args.response.data);
            if (!status.isOK()) {
                warning() << "killCursors command failed: " << redact(status);
            }
        };
        auto cmdObj = BSON("killCursors" << nss.coll() << "cursors" << BSON_ARRAY(id));
        auto scheduleResult = _executor->scheduleRemoteCommand(
            RemoteCommandRequest(_source, _dbname, cmdObj, nullptr), logKillCursorsResult);
        if (!scheduleResult.isOK()) {
            warning() << "failed to schedule killCursors command: "
                      << redact(scheduleResult.getStatus());
        }
    }
}
void Fetcher::_finishCallback() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _active = false;
    _first = false;
    _condition.notify_all();
}

}  // namespace mongo
