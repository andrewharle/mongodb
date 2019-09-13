
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

#include "mongo/util/net/hostandport.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class Status;

namespace repl {

/**
 * Arguments to the replSetHeartbeat command.
 */
class ReplSetHeartbeatArgsV1 {
public:
    /**
     * Initializes this ReplSetHeartbeatArgsV1 from the contents of args.
     */
    Status initialize(const BSONObj& argsObj);

    /**
     * Returns true if all required fields have been initialized.
     */
    bool isInitialized() const;

    /**
     * Gets the ReplSetConfig version number of the sender.
     */
    long long getConfigVersion() const {
        return _configVersion;
    }

    /**
     * Gets the heartbeat version number of the sender. This field was added to ensure that
     * heartbeats sent from featureCompatibilityVersion 3.6 nodes to binary version 3.4 nodes fail.
     */
    long long getHeartbeatVersion() const {
        return _heartbeatVersion;
    }

    /**
     * Gets the _id of the sender in their ReplSetConfig.
     */
    long long getSenderId() const {
        return _senderId;
    }

    /**
     * Gets the HostAndPort of the sender.
     */
    HostAndPort getSenderHost() const {
        return _senderHost;
    }

    /**
     * Gets the replSet name of the sender's replica set.
     */
    std::string getSetName() const {
        return _setName;
    }

    /**
     * Gets the term the sender believes it to be.
     */
    long long getTerm() const {
        return _term;
    }

    /**
     * Returns whether or not the sender is checking for emptiness.
     */
    bool hasCheckEmpty() const {
        return _checkEmpty;
    }

    /**
     * Returns whether or not the HostAndPort of the sender is set.
     */
    bool hasSender() const {
        return _hasSender;
    }

    /**
     * Returns whether or not the heartbeat version of the sender is set.
     */
    bool hasHeartbeatVersion() const {
        return _hasHeartbeatVersion;
    }

    /**
     * The below methods set the value in the method name to 'newVal'.
     */
    void setConfigVersion(long long newVal);
    void setHeartbeatVersion(long long newVal);
    void setSenderId(long long newVal);
    void setSenderHost(const HostAndPort& newVal);
    void setSetName(const std::string& newVal);
    void setTerm(long long newVal);
    void setCheckEmpty();

    /**
     * Returns a BSONified version of the object.
     * Should only be called if the mandatory fields have been set.
     * Optional fields are only included if they have been set.
     */
    BSONObj toBSON() const;

    void addToBSON(BSONObjBuilder* builder) const;

private:
    // look at the body of the isInitialized() function to see which fields are mandatory
    long long _configVersion = -1;
    long long _heartbeatVersion = -1;
    long long _senderId = -1;
    long long _term = -1;
    bool _checkEmpty = false;
    bool _hasSender = false;
    bool _hasHeartbeatVersion = false;
    std::string _setName;
    HostAndPort _senderHost;
};

}  // namespace repl
}  // namespace mongo
