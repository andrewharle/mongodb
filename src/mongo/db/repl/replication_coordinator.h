
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

#include "mongo/db/repl/replication_coordinator_fwd.h"

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/split_horizon.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class IndexDescriptor;
class NamespaceString;
class OperationContext;
class ServiceContext;
class Timestamp;
struct WriteConcernOptions;

namespace executor {
struct ConnectionPoolStats;
}  // namespace executor

namespace rpc {

class OplogQueryMetadata;
class ReplSetMetadata;

}  // namespace rpc

namespace repl {

class BackgroundSync;
class IsMasterResponse;
class OplogReader;
class OpTime;
class ReadConcernArgs;
class ReplSetConfig;
class ReplSetHeartbeatArgs;
class ReplSetHeartbeatArgsV1;
class ReplSetHeartbeatResponse;
class ReplSetHtmlSummary;
class ReplSetRequestVotesArgs;
class ReplSetRequestVotesResponse;
class UpdatePositionArgs;

/**
 * The ReplicationCoordinator is responsible for coordinating the interaction of replication
 * with the rest of the system.  The public methods on ReplicationCoordinator are the public
 * API that the replication subsystem presents to the rest of the codebase.
 */
class ReplicationCoordinator : public SyncSourceSelector {
    MONGO_DISALLOW_COPYING(ReplicationCoordinator);

public:
    static ReplicationCoordinator* get(ServiceContext* service);
    static ReplicationCoordinator* get(ServiceContext& service);
    static ReplicationCoordinator* get(OperationContext* ctx);

    static void set(ServiceContext* service,
                    std::unique_ptr<ReplicationCoordinator> replCoordinator);

    struct StatusAndDuration {
    public:
        Status status;
        Milliseconds duration;

        StatusAndDuration(const Status& stat, Milliseconds ms) : status(stat), duration(ms) {}
    };

    virtual ~ReplicationCoordinator();

    /**
     * Does any initial bookkeeping needed to start replication, and instructs the other
     * components of the replication system to start up whatever threads and do whatever
     * initialization they need.
     */
    virtual void startup(OperationContext* opCtx) = 0;

    /**
     * Start terminal shutdown.  This causes the topology coordinator to refuse to vote in any
     * further elections.  This should only be called from global shutdown after we've passed the
     * point of no return.
     *
     * This should be called once we are sure to call shutdown().
     */
    virtual void enterTerminalShutdown() = 0;

    /**
     * Does whatever cleanup is required to stop replication, including instructing the other
     * components of the replication system to shut down and stop any threads they are using,
     * blocking until all replication-related shutdown tasks are complete.
     */
    virtual void shutdown(OperationContext* opCtx) = 0;

    /**
     * Returns a reference to the parsed command line arguments that are related to replication.
     */
    virtual const ReplSettings& getSettings() const = 0;

    enum Mode { modeNone = 0, modeReplSet };

    /**
     * Returns a value indicating whether this node was configured at start-up to run standalone or
     * as a member of a replica set.
     */
    virtual Mode getReplicationMode() const = 0;

    /**
     * Returns true if this node is configured to be a member of a replica set.
     */
    virtual bool isReplEnabled() const = 0;

    /**
     * Returns the current replica set state of this node (PRIMARY, SECONDARY, STARTUP, etc).
     * It is invalid to call this unless getReplicationMode() == modeReplSet.
     */
    virtual MemberState getMemberState() const = 0;

    /**
     * Waits for 'timeout' ms for member state to become 'state'.
     * Returns OK if member state is 'state'.
     * Returns ErrorCodes::ExceededTimeLimit if we timed out waiting for the state change.
     * Returns ErrorCodes::BadValue if timeout is negative.
     */
    virtual Status waitForMemberState(MemberState expectedState, Milliseconds timeout) = 0;

    /**
     * Returns true if this node is in state PRIMARY or SECONDARY.
     *
     * It is invalid to call this unless getReplicationMode() == modeReplSet.
     *
     * This method may be optimized to reduce synchronization overhead compared to
     * reading the current member state with getMemberState().
     */
    virtual bool isInPrimaryOrSecondaryState() const = 0;


    /**
     * Returns how slave delayed this node is configured to be, or 0 seconds if this node is not a
     * member of the current replica set configuration.
     */
    virtual Seconds getSlaveDelaySecs() const = 0;

    /**
     * Blocks the calling thread for up to writeConcern.wTimeout millis, or until "opTime" has
     * been replicated to at least a set of nodes that satisfies the writeConcern, whichever
     * comes first. A writeConcern.wTimeout of 0 indicates no timeout (block forever) and a
     * writeConcern.wTimeout of -1 indicates return immediately after checking. Return codes:
     * ErrorCodes::WriteConcernFailed if the writeConcern.wTimeout is reached before
     *     the data has been sufficiently replicated
     * ErrorCodes::ExceededTimeLimit if the opCtx->getMaxTimeMicrosRemaining is reached before
     *     the data has been sufficiently replicated
     * ErrorCodes::NotMaster if the node is not Primary/Master
     * ErrorCodes::UnknownReplWriteConcern if the writeConcern.wMode contains a write concern
     *     mode that is not known
     * ErrorCodes::ShutdownInProgress if we are mid-shutdown
     * ErrorCodes::Interrupted if the operation was killed with killop()
     */
    virtual StatusAndDuration awaitReplication(OperationContext* opCtx,
                                               const OpTime& opTime,
                                               const WriteConcernOptions& writeConcern) = 0;

    /**
     * Causes this node to relinquish being primary for at least 'stepdownTime'.  If 'force' is
     * false, before doing so it will wait for 'waitTime' for one other node to be within 10
     * seconds of this node's optime before stepping down. Returns a Status with the code
     * ErrorCodes::ExceededTimeLimit if no secondary catches up within waitTime,
     * ErrorCodes::NotMaster if you are no longer primary when trying to step down,
     * ErrorCodes::SecondaryAheadOfPrimary if we are primary but there is another node that
     * seems to be ahead of us in replication, and Status::OK otherwise.
     */
    virtual Status stepDown(OperationContext* opCtx,
                            bool force,
                            const Milliseconds& waitTime,
                            const Milliseconds& stepdownTime) = 0;

    /**
     * Returns true if the node can be considered master for the purpose of introspective
     * commands such as isMaster() and rs.status().
     */
    virtual bool isMasterForReportingPurposes() = 0;

    /**
     * Returns true if it is valid for this node to accept writes on the given database.  Currently
     * this is true only if this node is Primary, a standalone, or is writing to the local database.
     *
     * If a node was started with the replSet argument, but has not yet received a config, it
     * will not be able to receive writes to a database other than local (it will not be
     * treated as standalone node).
     *
     * NOTE: This function can only be meaningfully called while the caller holds the global
     * lock in some mode other than MODE_NONE.
     */
    virtual bool canAcceptWritesForDatabase(OperationContext* opCtx, StringData dbName) = 0;

    /**
     * Version which does not check for the global lock.  Do not use in new code.
     * Without the global lock held, the return value may be inaccurate by the time
     * the function returns.
     */
    virtual bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx, StringData dbName) = 0;

    /**
     * Returns true if it is valid for this node to accept writes on the given namespace.
     *
     * The result of this function should be consistent with canAcceptWritesForDatabase()
     * for the database the namespace refers to, with additional checks on the collection.
     */
    virtual bool canAcceptWritesFor(OperationContext* opCtx, const NamespaceString& ns) = 0;

    /**
     * Version which does not check for the global lock.  Do not use in new code.
     * Without the global lock held, the return value may be inaccurate by the time
     * the function returns.
     */
    virtual bool canAcceptWritesFor_UNSAFE(OperationContext* opCtx, const NamespaceString& ns) = 0;

    /**
     * Checks if the current replica set configuration can satisfy the given write concern.
     *
     * Things that are taken into consideration include:
     * 1. If the set has enough data-bearing members.
     * 2. If the write concern mode exists.
     * 3. If there are enough members for the write concern mode specified.
     */
    virtual Status checkIfWriteConcernCanBeSatisfied(
        const WriteConcernOptions& writeConcern) const = 0;

    /**
     * Returns Status::OK() if it is valid for this node to serve reads on the given collection
     * and an errorcode indicating why the node cannot if it cannot.
     */
    virtual Status checkCanServeReadsFor(OperationContext* opCtx,
                                         const NamespaceString& ns,
                                         bool slaveOk) = 0;

    /**
     * Version which does not check for the global lock.  Do not use in new code.
     * Without the global lock held, the return value may be inaccurate by the time
     * the function returns.
     */
    virtual Status checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                const NamespaceString& ns,
                                                bool slaveOk) = 0;

    /**
     * Returns true if this node should ignore index constraints for idempotency reasons.
     *
     * The namespace "ns" is passed in because the "local" database is usually writable
     * and we need to enforce the constraints for it.
     */
    virtual bool shouldRelaxIndexConstraints(OperationContext* opCtx,
                                             const NamespaceString& ns) = 0;

    /**
     * Updates our internal tracking of the last OpTime applied to this node.
     *
     * The new value of "opTime" must be no less than any prior value passed to this method, and
     * it is the caller's job to properly synchronize this behavior.  The exception to this rule
     * is that after calls to resetLastOpTimesFromOplog(), the minimum acceptable value for
     * "opTime" is reset based on the contents of the oplog, and may go backwards due to
     * rollback. Additionally, the optime given MUST represent a consistent database state.
     */
    virtual void setMyLastAppliedOpTime(const OpTime& opTime) = 0;

    /**
     * Updates our internal tracking of the last OpTime durable to this node.
     *
     * The new value of "opTime" must be no less than any prior value passed to this method, and
     * it is the caller's job to properly synchronize this behavior.  The exception to this rule
     * is that after calls to resetLastOpTimesFromOplog(), the minimum acceptable value for
     * "opTime" is reset based on the contents of the oplog, and may go backwards due to
     * rollback.
     */
    virtual void setMyLastDurableOpTime(const OpTime& opTime) = 0;

    /**
     * This type is used to represent the "consistency" of a current database state. In
     * replication, there may be times when our database data is not represented by a single optime,
     * because we have fetched remote data from different points in time. For example, when we are
     * in RECOVERING following a refetch based rollback. We never allow external clients to read
     * from the database if it is not consistent.
     */
    enum class DataConsistency { Consistent, Inconsistent };

    /**
     * Updates our internal tracking of the last OpTime applied to this node, but only
     * if the supplied optime is later than the current last OpTime known to the replication
     * coordinator. The 'consistency' argument must tell whether or not the optime argument
     * represents a consistent database state.
     *
     * This function is used by logOp() on a primary, since the ops in the oplog do not
     * necessarily commit in sequential order. It is also used when we finish oplog batch
     * application on secondaries, to avoid any potential race conditions around setting the
     * applied optime from more than one thread.
     */
    virtual void setMyLastAppliedOpTimeForward(const OpTime& opTime,
                                               DataConsistency consistency) = 0;

    /**
     * Updates our internal tracking of the last OpTime durable to this node, but only
     * if the supplied optime is later than the current last OpTime known to the replication
     * coordinator.
     *
     * This function is used by logOp() on a primary, since the ops in the oplog do not
     * necessarily commit in sequential order.
     */
    virtual void setMyLastDurableOpTimeForward(const OpTime& opTime) = 0;

    /**
     * Same as above, but used during places we need to zero our last optime.
     */
    virtual void resetMyLastOpTimes() = 0;

    /**
     * Updates our the message we include in heartbeat responses.
     */
    virtual void setMyHeartbeatMessage(const std::string& msg) = 0;

    /**
     * Returns the last optime recorded by setMyLastAppliedOpTime.
     */
    virtual OpTime getMyLastAppliedOpTime() const = 0;

    /**
     * Returns the last optime recorded by setMyLastDurableOpTime.
     */
    virtual OpTime getMyLastDurableOpTime() const = 0;

    /**
     * Waits until the optime of the current node is at least the opTime specified in 'settings'.
     *
     * Returns whether the wait was successful.
     */
    virtual Status waitUntilOpTimeForRead(OperationContext* opCtx,
                                          const ReadConcernArgs& settings) = 0;

    /**
     * Waits until the deadline or until the optime of the current node is at least the opTime
     * specified in 'settings'.
     *
     * Returns whether the wait was successful.
     */
    virtual Status waitUntilOpTimeForReadUntil(OperationContext* opCtx,
                                               const ReadConcernArgs& settings,
                                               boost::optional<Date_t> deadline) = 0;

    /**
     * Retrieves and returns the current election id, which is a unique id that is local to
     * this node and changes every time we become primary.
     * TODO(spencer): Use term instead.
     */
    virtual OID getElectionId() = 0;

    /**
     * Returns the id for this node as specified in the current replica set configuration.
     */
    virtual int getMyId() const = 0;

    /**
     * Sets this node into a specific follower mode.
     *
     * Returns OK if the follower mode was successfully set.  Returns NotSecondary if the
     * node is a leader when setFollowerMode is called and ElectionInProgess if the node is in the
     * process of trying to elect itself primary.
     *
     * Follower modes are RS_STARTUP2 (initial sync), RS_SECONDARY, RS_ROLLBACK and
     * RS_RECOVERING.  They are the valid states of a node whose topology coordinator has the
     * follower role.
     *
     * This is essentially an interface that allows the applier to prevent the node from
     * becoming a candidate or accepting reads, depending on circumstances in the oplog
     * application process.
     */
    virtual Status setFollowerMode(const MemberState& newState) = 0;

    /**
     * Step-up
     * =======
     * On stepup, repl coord enters catch-up mode. It's the same as the secondary mode from
     * the perspective of producer and applier, so there's nothing to do with them.
     * When a node enters drain mode, producer state = Stopped, applier state = Draining.
     *
     * If the applier state is Draining, it will signal repl coord when there's nothing to apply.
     * The applier goes into Stopped state at the same time.
     *
     * The states go like the following:
     * - secondary and during catchup mode
     * (producer: Running, applier: Running)
     *      |
     *      | finish catch-up, enter drain mode
     *      V
     * - drain mode
     * (producer: Stopped, applier: Draining)
     *      |
     *      | applier signals drain is complete
     *      V
     * - primary is in master mode
     * (producer: Stopped, applier: Stopped)
     *
     *
     * Step-down
     * =========
     * The state transitions become:
     * - primary is in master mode
     * (producer: Stopped, applier: Stopped)
     *      |
     *      | step down
     *      V
     * - secondary mode, starting bgsync
     * (producer: Starting, applier: Running)
     *      |
     *      | bgsync runs start()
     *      V
     * - secondary mode, normal
     * (producer: Running, applier: Running)
     *
     * When a node steps down during draining mode, it's OK to change from (producer: Stopped,
     * applier: Draining) to (producer: Starting, applier: Running).
     *
     * When a node steps down during catchup mode, the states remain the same (producer: Running,
     * applier: Running).
     */
    enum class ApplierState { Running, Draining, Stopped };

    /**
     * In normal cases: Running -> Draining -> Stopped -> Running.
     * Draining -> Running is also possible if a node steps down during drain mode.
     *
     * Only the applier can make the transition from Draining to Stopped by calling
     * signalDrainComplete().
     */
    virtual ApplierState getApplierState() = 0;

    /**
     * Signals that a previously requested pause and drain of the applier buffer
     * has completed.
     *
     * This is an interface that allows the applier to reenable writes after
     * a successful election triggers the draining of the applier buffer.
     *
     * The applier signals drain complete when the buffer is empty and it's in Draining
     * state. We need to make sure the applier checks both conditions in the same term.
     * Otherwise, it's possible that the applier confirms the empty buffer, but the node
     * steps down and steps up so quickly that the applier signals drain complete in the wrong
     * term.
     */
    virtual void signalDrainComplete(OperationContext* opCtx, long long termWhenBufferIsEmpty) = 0;

    /**
     * Waits duration of 'timeout' for applier to finish draining its buffer of operations.
     * Returns OK if we are not in drain mode.
     * Returns ErrorCodes::ExceededTimeLimit if we timed out waiting for the applier to drain its
     * buffer.
     * Returns ErrorCodes::BadValue if timeout is negative.
     */
    virtual Status waitForDrainFinish(Milliseconds timeout) = 0;

    /**
     * Signals the sync source feedback thread to wake up and send a handshake and
     * replSetUpdatePosition command to our sync source.
     */
    virtual void signalUpstreamUpdater() = 0;

    /**
     * Prepares a BSONObj describing an invocation of the replSetUpdatePosition command that can
     * be sent to this node's sync source to update it about our progress in replication.
     */
    virtual StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const = 0;

    enum class ReplSetGetStatusResponseStyle { kBasic, kInitialSync };

    /**
     * Handles an incoming replSetGetStatus command. Adds BSON to 'result'. If kInitialSync is
     * requested but initial sync is not running, kBasic will be used.
     */
    virtual Status processReplSetGetStatus(BSONObjBuilder* result,
                                           ReplSetGetStatusResponseStyle responseStyle) = 0;

    /**
     * Does an initial sync of data, after dropping existing data.
     */
    virtual Status resyncData(OperationContext* opCtx, bool waitUntilCompleted) = 0;

    /**
     * Handles an incoming isMaster command for a replica set node.  Should not be
     * called on a standalone node.
     */
    virtual void fillIsMasterForReplSet(IsMasterResponse* result,
                                        const SplitHorizon::Parameters& horizonParams) = 0;

    /**
     * Adds to "result" a description of the slaveInfo data structure used to map RIDs to their
     * last known optimes.
     */
    virtual void appendSlaveInfoData(BSONObjBuilder* result) = 0;

    /**
     * Returns a copy of the current ReplSetConfig.
     */
    virtual ReplSetConfig getConfig() const = 0;

    /**
     * Handles an incoming replSetGetConfig command. Adds BSON to 'result'.
     */
    virtual void processReplSetGetConfig(BSONObjBuilder* result) = 0;

    /**
     * Processes the ReplSetMetadata returned from a command run against another
     * replica set member and so long as the config version in the metadata matches the replica set
     * config version this node currently has, updates the current term.
     *
     * This does NOT update this node's notion of the commit point.
     */
    virtual void processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) = 0;

    /**
     * This updates the node's notion of the commit point.
     */
    virtual void advanceCommitPoint(const OpTime& committedOptime) = 0;

    /**
     * Elections under protocol version 1 are triggered by a timer.
     * When a node is informed of the primary's liveness (either through heartbeats or
     * while reading a sync source's oplog), it calls this function to postpone the
     * election timer by a duration of at least 'electionTimeoutMillis' (see getConfig()).
     * If the current node is not electable (secondary with priority > 0), this function
     * cancels the existing timer but will not schedule a new one.
     */
    virtual void cancelAndRescheduleElectionTimeout() = 0;

    /**
     * Toggles maintenanceMode to the value expressed by 'activate'
     * return Status::OK if the change worked, NotSecondary if it failed because we are
     * PRIMARY, and OperationFailed if we are not currently in maintenance mode
     */
    virtual Status setMaintenanceMode(bool activate) = 0;

    /**
     * Retrieves the current count of maintenanceMode and returns 'true' if greater than 0.
     */
    virtual bool getMaintenanceMode() = 0;

    /**
     * Handles an incoming replSetSyncFrom command. Adds BSON to 'result'
     * returns Status::OK if the sync target could be set and an ErrorCode indicating why it
     * couldn't otherwise.
     */
    virtual Status processReplSetSyncFrom(OperationContext* opCtx,
                                          const HostAndPort& target,
                                          BSONObjBuilder* resultObj) = 0;

    /**
     * Handles an incoming replSetFreeze command. Adds BSON to 'resultObj'
     * returns Status::OK() if the node is a member of a replica set with a config and an
     * error Status otherwise
     */
    virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj) = 0;

    /**
     * Handles an incoming heartbeat command with arguments 'args'. Populates 'response';
     * returns a Status with either OK or an error message.
     */
    virtual Status processHeartbeat(const ReplSetHeartbeatArgs& args,
                                    ReplSetHeartbeatResponse* response) = 0;
    virtual Status processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                      ReplSetHeartbeatResponse* response) = 0;


    /**
     * Arguments for the replSetReconfig command.
     */
    struct ReplSetReconfigArgs {
        BSONObj newConfigObj;
        bool force;
    };

    /**
     * Handles an incoming replSetReconfig command. Adds BSON to 'resultObj';
     * returns a Status with either OK or an error message.
     */
    virtual Status processReplSetReconfig(OperationContext* opCtx,
                                          const ReplSetReconfigArgs& args,
                                          BSONObjBuilder* resultObj) = 0;

    /*
     * Handles an incoming replSetInitiate command. If "configObj" is empty, generates a default
     * configuration to use.
     * Adds BSON to 'resultObj'; returns a Status with either OK or an error message.
     */
    virtual Status processReplSetInitiate(OperationContext* opCtx,
                                          const BSONObj& configObj,
                                          BSONObjBuilder* resultObj) = 0;

    /**
     * Arguments to the replSetFresh command.
     */
    struct ReplSetFreshArgs {
        std::string setName;  // Name of the replset
        HostAndPort who;      // host and port of the member that sent the replSetFresh command
        unsigned id;          // replSet id of the member that sent the replSetFresh command
        int cfgver;  // replSet config version that the member who sent the command thinks it has
        Timestamp opTime;  // last optime seen by the member who sent the replSetFresh command
    };

    /*
     * Handles an incoming replSetFresh command.
     * Adds BSON to 'resultObj'; returns a Status with either OK or an error message.
     */
    virtual Status processReplSetFresh(const ReplSetFreshArgs& args, BSONObjBuilder* resultObj) = 0;

    /**
     * Arguments to the replSetElect command.
     */
    struct ReplSetElectArgs {
        std::string set;  // Name of the replset
        int whoid;        // replSet id of the member that sent the replSetFresh command
        int cfgver;  // replSet config version that the member who sent the command thinks it has
        OID round;   // unique ID for this election
    };

    /*
     * Handles an incoming replSetElect command.
     * Adds BSON to 'resultObj'; returns a Status with either OK or an error message.
     */
    virtual Status processReplSetElect(const ReplSetElectArgs& args, BSONObjBuilder* resultObj) = 0;

    /**
     * Handles an incoming replSetUpdatePosition command, updating each node's oplog progress.
     * Returns Status::OK() if all updates are processed correctly, NodeNotFound
     * if any updating node cannot be found in the config, InvalidReplicaSetConfig if the
     * "configVersion" sent in any of the updates doesn't match our config version, or
     * NotMasterOrSecondary if we are in state REMOVED or otherwise don't have a valid
     * replica set config.
     * If a non-OK status is returned, it is unspecified whether none or some of the updates
     * were applied.
     * "configVersion" will be populated with our config version if and only if we return
     * InvalidReplicaSetConfig.
     */
    virtual Status processReplSetUpdatePosition(const UpdatePositionArgs& updates,
                                                long long* configVersion) = 0;

    /**
     * Returns a bool indicating whether or not this node builds indexes.
     */
    virtual bool buildsIndexes() = 0;

    /**
     * Returns a vector of members that have applied the operation with OpTime 'op'.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     */
    virtual std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op, bool durablyWritten) = 0;

    /**
     * Returns a vector of the members other than ourself in the replica set, as specified in
     * the replica set config.  Invalid to call if we are not in replica set mode.  Returns
     * an empty vector if we do not have a valid config.
     */
    virtual std::vector<HostAndPort> getOtherNodesInReplSet() const = 0;

    /**
     * Returns a BSONObj containing a representation of the current default write concern.
     */
    virtual WriteConcernOptions getGetLastErrorDefault() = 0;

    /**
     * Checks that the --replSet flag was passed when starting up the node and that the node
     * has a valid replica set config.
     *
     * Returns a Status indicating whether those conditions are met with errorcode
     * NoReplicationEnabled if --replSet was not present during start up or with errorcode
     * NotYetInitialized in the absence of a valid config. Also adds error info to "result".
     */
    virtual Status checkReplEnabledForCommand(BSONObjBuilder* result) = 0;

    /**
     * Loads the optime from the last op in the oplog into the coordinator's lastAppliedOpTime and
     * lastDurableOpTime values. The 'consistency' argument must tell whether or not the optime of
     * the op in the oplog represents a consistent database state.
     */
    virtual void resetLastOpTimesFromOplog(OperationContext* opCtx,
                                           DataConsistency consistency) = 0;

    /**
     * Returns the OpTime of the latest replica set-committed op known to this server.
     * Committed means a majority of the voting nodes of the config are known to have the
     * operation in their oplogs.  This implies such ops will never be rolled back.
     */
    virtual OpTime getLastCommittedOpTime() const = 0;

    /*
    * Handles an incoming replSetRequestVotes command.
    *
    * Populates the given 'response' object with the result of the request. If there is a failure
    * processing the vote request, returns an error status. If an error is returned, the value of
    * the populated 'response' object is invalid.
    */
    virtual Status processReplSetRequestVotes(OperationContext* opCtx,
                                              const ReplSetRequestVotesArgs& args,
                                              ReplSetRequestVotesResponse* response) = 0;

    /**
     * Prepares a metadata object with the ReplSetMetadata and the OplogQueryMetadata depending
     * on what has been requested.
     */
    virtual void prepareReplMetadata(const BSONObj& metadataRequestObj,
                                     const OpTime& lastOpTimeFromClient,
                                     BSONObjBuilder* builder) const = 0;

    /**
     * Returns true if the V1 election protocol is being used and false otherwise.
     */
    virtual bool isV1ElectionProtocol() const = 0;

    /**
     * Returns whether or not majority write concerns should implicitly journal, if j has not been
     * explicitly set.
     */
    virtual bool getWriteConcernMajorityShouldJournal() = 0;

    /**
     * Writes into 'output' all the information needed to generate a summary of the current
     * replication state for use by the web interface.
     */
    virtual void summarizeAsHtml(ReplSetHtmlSummary* output) = 0;

    /**
     * Returns the current term.
     */
    virtual long long getTerm() = 0;

    /**
     * Attempts to update the current term for the V1 election protocol. If the term changes and
     * this node is primary, relinquishes primary.
     * Returns a Status OK if the term was *not* updated (meaning, it is safe to proceed with
     * the rest of the work, because the term is still the same).
     * Returns StaleTerm if the supplied term was higher than the current term.
     */
    virtual Status updateTerm(OperationContext* opCtx, long long term) = 0;

    /**
     * Blocks until either the current committed snapshot is at least as high as 'untilSnapshot',
     * or we are interrupted for any reason, including shutdown or maxTimeMs expiration.
     * 'opCtx' is used to checkForInterrupt and enforce maxTimeMS.
     */
    virtual void waitUntilSnapshotCommitted(OperationContext* opCtx,
                                            const Timestamp& untilSnapshot) = 0;

    /**
     * Resets all information related to snapshotting.
     */
    virtual void dropAllSnapshots() = 0;

    /**
     * Gets the latest OpTime of the currentCommittedSnapshot.
     */
    virtual OpTime getCurrentCommittedSnapshotOpTime() const = 0;

    /**
     * Appends diagnostics about the replication subsystem.
     */
    virtual void appendDiagnosticBSON(BSONObjBuilder* bob) = 0;

    /**
     * Appends connection information to the provided BSONObjBuilder.
     */
    virtual void appendConnectionStats(executor::ConnectionPoolStats* stats) const = 0;

    /**
     * Gets the number of uncommitted snapshots currently held.
     * Warning: This value can change at any time and may not even be accurate at the time of
     * return. It should not be used when an exact amount is needed.
     */
    virtual size_t getNumUncommittedSnapshots() = 0;

    /**
     * Creates a CallbackWaiter that waits for w:majority write concern to be satisfied up to opTime
     * before setting the 'wMajorityWriteAvailabilityDate' election candidate metric.
     */
    virtual void createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) = 0;

    /**
     * Returns a new WriteConcernOptions based on "wc" but with UNSET syncMode reset to JOURNAL or
     * NONE based on our rsConfig.
     */
    virtual WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(
        WriteConcernOptions wc) = 0;
    virtual ReplSettings::IndexPrefetchConfig getIndexPrefetchConfig() const = 0;
    virtual void setIndexPrefetchConfig(const ReplSettings::IndexPrefetchConfig cfg) = 0;

    virtual Status stepUpIfEligible(bool skipDryRun) = 0;

    virtual ServiceContext* getServiceContext() = 0;

    enum PrimaryCatchUpConclusionReason {
        kSucceeded,
        kAlreadyCaughtUp,
        kSkipped,
        kTimedOut,
        kFailedWithError,
        kFailedWithNewTerm,
        kFailedWithReplSetAbortPrimaryCatchUpCmd
    };

    /**
     * Abort catchup if the node is in catchup mode.
     */
    virtual Status abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) = 0;

    /**
     * Increment the counter for the number of ops applied during catchup if the node is in catchup
     * mode.
     */
    virtual void incrementNumCatchUpOpsIfCatchingUp(long numOps) = 0;

    /**
     * Signals that drop pending collections have been removed from storage.
     */
    virtual void signalDropPendingCollectionsRemovedFromStorage() = 0;

    /**
     * Returns true if logOp() should not append an entry to the oplog for the namespace for this
     * operation.
     */
    bool isOplogDisabledFor(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Field name of the newPrimaryMsg within the 'o' field in the new term oplog entry.
     */
    static const StringData newPrimaryMsgField;

    /**
     * Message string passed in the new term oplog entry after a primary has stepped up.
     */
    static const StringData newPrimaryMsg;

protected:
    ReplicationCoordinator();
};

}  // namespace repl
}  // namespace mongo
