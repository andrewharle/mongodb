
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/session_update_tracker.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

boost::optional<std::vector<OplogEntry>> SessionUpdateTracker::updateOrFlush(
    const OplogEntry& entry) {
    const auto& ns = entry.getNamespace();

    if (ns == NamespaceString::kSessionTransactionsTableNamespace ||
        (ns.isConfigDB() && ns.isCommand())) {
        return _flush(entry);
    }

    _updateSessionInfo(entry);
    return boost::none;
}

void SessionUpdateTracker::_updateSessionInfo(const OplogEntry& entry) {
    const auto& sessionInfo = entry.getOperationSessionInfo();

    if (!sessionInfo.getTxnNumber()) {
        return;
    }

    const auto& lsid = sessionInfo.getSessionId();
    invariant(lsid);

    // Ignore any no-op oplog entries, except for the ones generated from session migration
    // of CRUD ops. These entries will have an o2 field that contains the original CRUD
    // oplog entry.
    if (entry.getOpType() == OpTypeEnum::kNoop) {
        if (!entry.getFromMigrate() || !*entry.getFromMigrate()) {
            return;
        }

        if (!entry.getObject2()) {
            return;
        }
    }

    auto iter = _sessionsToUpdate.find(*lsid);
    if (iter == _sessionsToUpdate.end()) {
        _sessionsToUpdate.emplace(*lsid, entry);
        return;
    }

    const auto& existingSessionInfo = iter->second.getOperationSessionInfo();
    if (*sessionInfo.getTxnNumber() >= *existingSessionInfo.getTxnNumber()) {
        iter->second = entry;
        return;
    }

    severe() << "Entry for session " << lsid->getId() << " has txnNumber "
             << *sessionInfo.getTxnNumber() << " < " << *existingSessionInfo.getTxnNumber();
    severe() << "New oplog entry: " << redact(entry.toString());
    severe() << "Existing oplog entry: " << redact(iter->second.toString());

    fassertFailedNoTrace(50843);
}

std::vector<OplogEntry> SessionUpdateTracker::_flush(const OplogEntry& entry) {
    switch (entry.getOpType()) {
        case OpTypeEnum::kInsert:
        case OpTypeEnum::kNoop:
            // Session table is keyed by session id, so nothing to do here because
            // it would have triggered a unique index violation in the primary if
            // it was trying to insert with the same session id with existing ones.
            return {};

        case OpTypeEnum::kUpdate:
            return _flushForQueryPredicate(*entry.getObject2());

        case OpTypeEnum::kDelete:
            return _flushForQueryPredicate(entry.getObject());

        case OpTypeEnum::kCommand:
            return flushAll();
    }

    MONGO_UNREACHABLE;
}

std::vector<OplogEntry> SessionUpdateTracker::flushAll() {
    std::vector<OplogEntry> opList;

    for (auto&& entry : _sessionsToUpdate) {
        auto newUpdate = Session::createMatchingTransactionTableUpdate(entry.second);
        invariant(newUpdate);
        opList.push_back(std::move(*newUpdate));
    }

    _sessionsToUpdate.clear();

    return opList;
}

std::vector<OplogEntry> SessionUpdateTracker::_flushForQueryPredicate(
    const BSONObj& queryPredicate) {
    auto idField = queryPredicate["_id"].Obj();
    auto lsid = LogicalSessionId::parse(IDLParserErrorContext("lsidInOplogQuery"), idField);
    auto iter = _sessionsToUpdate.find(lsid);

    if (iter == _sessionsToUpdate.end()) {
        return {};
    }

    std::vector<OplogEntry> opList;
    auto updateOplog = Session::createMatchingTransactionTableUpdate(iter->second);
    invariant(updateOplog);
    opList.push_back(std::move(*updateOplog));
    _sessionsToUpdate.erase(iter);

    return opList;
}

}  // namespace repl
}  // namespace mongo
