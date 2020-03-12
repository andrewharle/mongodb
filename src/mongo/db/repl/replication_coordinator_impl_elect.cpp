
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

namespace {
class LoseElectionGuard {
    MONGO_DISALLOW_COPYING(LoseElectionGuard);

public:
    LoseElectionGuard(TopologyCoordinator* topCoord,
                      executor::TaskExecutor* executor,
                      std::unique_ptr<FreshnessChecker>* freshnessChecker,
                      std::unique_ptr<ElectCmdRunner>* electCmdRunner,
                      executor::TaskExecutor::EventHandle* electionFinishedEvent)
        : _topCoord(topCoord),
          _executor(executor),
          _freshnessChecker(freshnessChecker),
          _electCmdRunner(electCmdRunner),
          _electionFinishedEvent(electionFinishedEvent),
          _dismissed(false) {}

    ~LoseElectionGuard() {
        if (_dismissed) {
            return;
        }
        _topCoord->processLoseElection();
        _freshnessChecker->reset(NULL);
        _electCmdRunner->reset(NULL);
        if (_electionFinishedEvent->isValid()) {
            _executor->signalEvent(*_electionFinishedEvent);
        }
    }

    void dismiss() {
        _dismissed = true;
    }

private:
    TopologyCoordinator* const _topCoord;
    executor::TaskExecutor* const _executor;
    std::unique_ptr<FreshnessChecker>* const _freshnessChecker;
    std::unique_ptr<ElectCmdRunner>* const _electCmdRunner;
    const executor::TaskExecutor::EventHandle* _electionFinishedEvent;
    bool _dismissed;
};

}  // namespace

void ReplicationCoordinatorImpl::_startElectSelf_inlock() {
    invariant(!_freshnessChecker);
    invariant(!_electCmdRunner);

    switch (_rsConfigState) {
        case kConfigSteady:
            break;
        case kConfigInitiating:
        case kConfigReconfiguring:
        case kConfigHBReconfiguring:
            LOG(2) << "Not standing for election; processing a configuration change";
            // Transition out of candidate role.
            _topCoord->processLoseElection();
            return;
        default:
            severe() << "Entered replica set election code while in illegal config state "
                     << int(_rsConfigState);
            fassertFailed(18913);
    }

    log() << "Standing for election";
    const StatusWith<executor::TaskExecutor::EventHandle> finishEvh = _replExecutor->makeEvent();
    if (finishEvh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(18680, finishEvh.getStatus());
    _electionFinishedEvent = finishEvh.getValue();
    LoseElectionGuard lossGuard(_topCoord.get(),
                                _replExecutor.get(),
                                &_freshnessChecker,
                                &_electCmdRunner,
                                &_electionFinishedEvent);


    invariant(_rsConfig.getMemberAt(_selfIndex).isElectable());
    OpTime lastOpTimeApplied(_getMyLastAppliedOpTime_inlock());

    if (lastOpTimeApplied.isNull()) {
        log() << "not trying to elect self, "
                 "do not yet have a complete set of data from any point in time"
                 " -- lastAppliedOpTime is null";
        return;
    }

    _freshnessChecker.reset(new FreshnessChecker);

    StatusWith<executor::TaskExecutor::EventHandle> nextPhaseEvh =
        _freshnessChecker->start(_replExecutor.get(),
                                 lastOpTimeApplied.getTimestamp(),
                                 _rsConfig,
                                 _selfIndex,
                                 _topCoord->getMaybeUpHostAndPorts());
    if (nextPhaseEvh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(18681, nextPhaseEvh.getStatus());
    _replExecutor
        ->onEvent(nextPhaseEvh.getValue(),
                  [this](const mongo::executor::TaskExecutor::CallbackArgs&) {
                      _onFreshnessCheckComplete();
                  })
        .status_with_transitional_ignore();
    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::_onFreshnessCheckComplete() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_freshnessChecker);
    invariant(!_electCmdRunner);
    LoseElectionGuard lossGuard(_topCoord.get(),
                                _replExecutor.get(),
                                &_freshnessChecker,
                                &_electCmdRunner,
                                &_electionFinishedEvent);

    if (_freshnessChecker->isCanceled()) {
        LOG(2) << "Election canceled during freshness check phase";
        return;
    }

    const Date_t now(_replExecutor->now());
    const FreshnessChecker::ElectionAbortReason abortReason =
        _freshnessChecker->shouldAbortElection();

    // need to not sleep after last time sleeping,
    switch (abortReason) {
        case FreshnessChecker::None:
            break;
        case FreshnessChecker::FreshnessTie:
            if ((_selfIndex != 0) && !_sleptLastElection) {
                const auto ms = Milliseconds(_nextRandomInt64_inlock(1000) + 50);
                const Date_t nextCandidateTime = now + ms;
                log() << "possible election tie; sleeping " << ms << " until "
                      << dateToISOStringLocal(nextCandidateTime);
                _topCoord->setElectionSleepUntil(nextCandidateTime);
                _scheduleWorkAt(nextCandidateTime,
                                [=](const executor::TaskExecutor::CallbackArgs& cbData) {
                                    _recoverFromElectionTie(cbData);
                                });
                _sleptLastElection = true;
                return;
            }
            _sleptLastElection = false;
            break;
        case FreshnessChecker::FresherNodeFound:
            log() << "not electing self, we are not freshest";
            return;
        case FreshnessChecker::QuorumUnreachable:
            log() << "not electing self, we could not contact enough voting members";
            return;
        default:
            log() << "not electing self due to election abort message :"
                  << static_cast<int>(abortReason);
            return;
    }

    log() << "running for election"
          << (abortReason == FreshnessChecker::FreshnessTie
                  ? "; slept last election, so running regardless of possible tie"
                  : "");

    // Secure our vote for ourself first
    if (!_topCoord->voteForMyself(now)) {
        return;
    }

    _electCmdRunner.reset(new ElectCmdRunner);
    StatusWith<executor::TaskExecutor::EventHandle> nextPhaseEvh = _electCmdRunner->start(
        _replExecutor.get(), _rsConfig, _selfIndex, _topCoord->getMaybeUpHostAndPorts());
    if (nextPhaseEvh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(18685, nextPhaseEvh.getStatus());

    _replExecutor
        ->onEvent(nextPhaseEvh.getValue(),
                  [=](const executor::TaskExecutor::CallbackArgs&) { _onElectCmdRunnerComplete(); })
        .status_with_transitional_ignore();
    lossGuard.dismiss();
}

void ReplicationCoordinatorImpl::_onElectCmdRunnerComplete() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    LoseElectionGuard lossGuard(_topCoord.get(),
                                _replExecutor.get(),
                                &_freshnessChecker,
                                &_electCmdRunner,
                                &_electionFinishedEvent);

    invariant(_freshnessChecker);
    invariant(_electCmdRunner);
    if (_electCmdRunner->isCanceled()) {
        LOG(2) << "Election canceled during elect self phase";
        return;
    }

    const int receivedVotes = _electCmdRunner->getReceivedVotes();

    if (receivedVotes < _rsConfig.getMajorityVoteCount()) {
        log() << "couldn't elect self, only received " << receivedVotes
              << " votes, but needed at least " << _rsConfig.getMajorityVoteCount();
        // Suppress ourselves from standing for election again, giving other nodes a chance
        // to win their elections.
        const auto ms = Milliseconds(_nextRandomInt64_inlock(1000) + 50);
        const Date_t now(_replExecutor->now());
        const Date_t nextCandidateTime = now + ms;
        log() << "waiting until " << nextCandidateTime << " before standing for election again";
        _topCoord->setElectionSleepUntil(nextCandidateTime);
        _scheduleWorkAt(nextCandidateTime, [=](const executor::TaskExecutor::CallbackArgs& cbData) {
            _recoverFromElectionTie(cbData);
        });
        return;
    }

    if (_rsConfig.getConfigVersion() != _freshnessChecker->getOriginalConfigVersion()) {
        log() << "config version changed during our election, ignoring result";
        return;
    }

    log() << "election succeeded, assuming primary role";

    lossGuard.dismiss();
    _freshnessChecker.reset(NULL);
    _electCmdRunner.reset(NULL);
    auto electionFinishedEvent = _electionFinishedEvent;
    _postWonElectionUpdateMemberState_inlock();
    _replExecutor->signalEvent(electionFinishedEvent);
}

void ReplicationCoordinatorImpl::_recoverFromElectionTie(
    const executor::TaskExecutor::CallbackArgs& cbData) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto now = _replExecutor->now();
    const auto status = _topCoord->checkShouldStandForElection(now);
    if (!status.isOK()) {
        LOG(2) << "ReplicationCoordinatorImpl::_recoverFromElectionTie -- " << status.reason();
    } else {
        fassert(
            28817,
            _topCoord->becomeCandidateIfElectable(now, StartElectionReasonEnum::kElectionTimeout));
        _startElectSelf_inlock();
    }
}

}  // namespace repl
}  // namespace mongo
