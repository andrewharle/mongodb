
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

#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

/**
 * Description of actions taken in response to a heartbeat.
 *
 * This includes when to schedule the next heartbeat to a target, and any other actions to
 * take, such as scheduling an election or stepping down as primary.
 */
class HeartbeatResponseAction {
public:
    /**
     * Actions taken based on heartbeat responses
     */
    enum Action {
        NoAction,
        Reconfig,
        StartElection,
        StepDownSelf,
        StepDownRemotePrimary,
        PriorityTakeover,
        CatchupTakeover
    };

    /**
     * Makes a new action representing doing nothing.
     */
    static HeartbeatResponseAction makeNoAction();

    /**
     * Makes a new action representing the instruction to reconfigure the current node.
     */
    static HeartbeatResponseAction makeReconfigAction();

    /**
     * Makes a new action telling the current node to attempt to elect itself primary.
     */
    static HeartbeatResponseAction makeElectAction();

    /**
     * Makes a new action telling the current node to schedule an event to attempt to elect itself
     * primary after the appropriate priority takeover delay.
     */
    static HeartbeatResponseAction makePriorityTakeoverAction();

    /**
     * Makes a new action telling the current node to schedule an event to attempt to elect itself
     * primary after the appropriate catchup takeover delay.
     */
    static HeartbeatResponseAction makeCatchupTakeoverAction();

    /**
     * Makes a new action telling the current node to step down as primary.
     *
     * It is an error to call this with primaryIndex != the index of the current node.
     */
    static HeartbeatResponseAction makeStepDownSelfAction(int primaryIndex);

    /**
     * Makes a new action telling the current node to ask the specified remote node to step
     * down as primary.
     *
     * It is an error to call this with primaryIndex == the index of the current node.
     */
    static HeartbeatResponseAction makeStepDownRemoteAction(int primaryIndex);

    /**
     * Construct an action with unspecified action and a next heartbeat start date in the
     * past.
     */
    HeartbeatResponseAction();

    /**
     * Sets the date at which the next heartbeat should be scheduled.
     */
    void setNextHeartbeatStartDate(Date_t when);

    /**
     * Sets whether or not the heartbeat response advanced the member's opTime.
     */
    void setAdvancedOpTime(bool advanced);

    /**
     * Gets the action type of this action.
     */
    Action getAction() const {
        return _action;
    }

    /**
     * Gets the time at which the next heartbeat should be scheduled.  If the
     * time is not in the future, the next heartbeat should be scheduled immediately.
     */
    Date_t getNextHeartbeatStartDate() const {
        return _nextHeartbeatStartDate;
    }

    /**
     * If getAction() returns StepDownSelf or StepDownPrimary, this is the index
     * in the current replica set config of the node that ought to step down.
     */
    int getPrimaryConfigIndex() const {
        return _primaryIndex;
    }

    /*
     * Returns true if the heartbeat response resulting in our conception of the
     * member's optime moving forward, so we need to recalculate lastCommittedOpTime.
     */
    bool getAdvancedOpTime() const {
        return _advancedOpTime;
    }

private:
    Action _action;
    int _primaryIndex;
    Date_t _nextHeartbeatStartDate;
    bool _advancedOpTime = false;
};

}  // namespace repl
}  // namespace mongo
