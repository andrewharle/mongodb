
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

#include "mongo/db/jsobj.h"

namespace mongo {

class Status;

struct WriteConcernOptions {
public:
    enum class SyncMode { UNSET, NONE, FSYNC, JOURNAL };

    static const int kNoTimeout;
    static const int kNoWaiting;

    static const BSONObj Default;
    static const BSONObj Acknowledged;
    static const BSONObj Unacknowledged;
    static const BSONObj Majority;

    static const StringData kWriteConcernField;
    static const char kMajority[];  // = "majority"

    static const Seconds kWriteConcernTimeoutSystem;
    static const Seconds kWriteConcernTimeoutMigration;
    static const Seconds kWriteConcernTimeoutSharding;
    static const Seconds kWriteConcernTimeoutUserCommand;

    WriteConcernOptions() {
        reset();
    }

    WriteConcernOptions(int numNodes, SyncMode sync, int timeout);

    WriteConcernOptions(int numNodes, SyncMode sync, Milliseconds timeout);

    WriteConcernOptions(const std::string& mode, SyncMode sync, int timeout);

    WriteConcernOptions(const std::string& mode, SyncMode sync, Milliseconds timeout);

    Status parse(const BSONObj& obj);

    /**
     * Attempts to extract a writeConcern from cmdObj.
     * Verifies that the writeConcern is of type Object (BSON type).
     */
    static StatusWith<WriteConcernOptions> extractWCFromCommand(
        const BSONObj& cmdObj, const WriteConcernOptions& defaultWC = WriteConcernOptions());

    /**
     * Return true if the server needs to wait for other secondary nodes to satisfy this
     * write concern setting. Errs on the false positive for non-empty wMode.
     */
    bool shouldWaitForOtherNodes() const;

    void reset() {
        syncMode = SyncMode::UNSET;
        wNumNodes = 0;
        wMode = "";
        wTimeout = 0;
    }

    // Returns the BSON representation of this object.
    // Warning: does not return the same object passed on the last parse() call.
    BSONObj toBSON() const;

    SyncMode syncMode;

    // The w parameter for this write concern. The wMode represents the string format and
    // takes precedence over the numeric format wNumNodes.
    int wNumNodes;
    std::string wMode;

    // Timeout in milliseconds.
    int wTimeout;
    // Deadline. If this is set to something other than Date_t::max(), this takes precedence over
    // wTimeout.
    Date_t wDeadline = Date_t::max();

    // True if the default write concern was used.
    bool usedDefault = false;

    // True if the default 'w' value of w:1 was used.
    bool usedDefaultW = false;
};

}  // namespace mongo
