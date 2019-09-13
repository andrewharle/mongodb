
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

#include <string>

#include "mongo/base/status_with.h"
#include "mongo/db/logical_session_id_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/uuid.h"

namespace mongo {

using TxnNumber = std::int64_t;
using StmtId = std::int32_t;

// Default value for unassigned statementId.
const StmtId kUninitializedStmtId = -1;

// Used as a substitute statementId for oplog entries that were truncated and lost.
const StmtId kIncompleteHistoryStmtId = -2;

const TxnNumber kUninitializedTxnNumber = -1;

class BSONObjBuilder;
class OperationContext;

const Minutes kLogicalSessionDefaultTimeout = Minutes(30);
extern int localLogicalSessionTimeoutMinutes;

inline bool operator==(const LogicalSessionId& lhs, const LogicalSessionId& rhs) {
    auto makeEqualityLens = [](const auto& lsid) { return std::tie(lsid.getId(), lsid.getUid()); };

    return makeEqualityLens(lhs) == makeEqualityLens(rhs);
}

inline bool operator!=(const LogicalSessionId& lhs, const LogicalSessionId& rhs) {
    return !(lhs == rhs);
}

inline bool operator==(const LogicalSessionRecord& lhs, const LogicalSessionRecord& rhs) {
    return lhs.getId() == rhs.getId();
}

inline bool operator!=(const LogicalSessionRecord& lhs, const LogicalSessionRecord& rhs) {
    return !(lhs == rhs);
}

LogicalSessionId makeLogicalSessionIdForTest();

LogicalSessionRecord makeLogicalSessionRecordForTest();

struct LogicalSessionIdHash {
    std::size_t operator()(const LogicalSessionId& lsid) const {
        return _hasher(lsid.getId());
    }

private:
    UUID::Hash _hasher;
};

struct LogicalSessionRecordHash {
    std::size_t operator()(const LogicalSessionRecord& lsid) const {
        return _hasher(lsid.getId().getId());
    }

private:
    UUID::Hash _hasher;
};


inline std::ostream& operator<<(std::ostream& s, const LogicalSessionId& lsid) {
    return (s << lsid.getId() << " - " << lsid.getUid());
}

inline StringBuilder& operator<<(StringBuilder& s, const LogicalSessionId& lsid) {
    return (s << lsid.getId().toString() << " - " << lsid.getUid().toString());
}

inline std::ostream& operator<<(std::ostream& s, const LogicalSessionFromClient& lsid) {
    return (s << lsid.getId() << " - " << (lsid.getUid() ? lsid.getUid()->toString() : ""));
}

inline StringBuilder& operator<<(StringBuilder& s, const LogicalSessionFromClient& lsid) {
    return (s << lsid.getId() << " - " << (lsid.getUid() ? lsid.getUid()->toString() : ""));
}

/**
 * An alias for sets of session ids.
 */
using LogicalSessionIdSet = stdx::unordered_set<LogicalSessionId, LogicalSessionIdHash>;
using LogicalSessionRecordSet = stdx::unordered_set<LogicalSessionRecord, LogicalSessionRecordHash>;

template <typename T>
using LogicalSessionIdMap = stdx::unordered_map<LogicalSessionId, T, LogicalSessionIdHash>;

}  // namespace mongo
