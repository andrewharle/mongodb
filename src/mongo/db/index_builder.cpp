
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index_builder.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::endl;

AtomicUInt32 IndexBuilder::_indexBuildCount;

namespace {

/**
 * Returns true if writes to the catalog entry for the input namespace require being
 * timestamped. A ghost write is when the operation is not committed with an oplog entry and
 * implies the caller will look at the logical clock to choose a time to use.
 */
bool requiresGhostCommitTimestamp(OperationContext* opCtx, NamespaceString nss) {
    if (!nss.isReplicated() || nss.coll().startsWith("tmp.mr.")) {
        return false;
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->getSettings().usingReplSets()) {
        return false;
    }

    // If there is a commit timestamp already assigned, there's no need to explicitly assign a
    // timestamp. This case covers foreground index builds.
    if (!opCtx->recoveryUnit()->getCommitTimestamp().isNull()) {
        return false;
    }

    // Only oplog entries (including a user's `applyOps` command) construct indexes via
    // `IndexBuilder`. Nodes in `startup` may not yet have initialized the `LogicalClock`, however
    // index builds during startup replication recovery must be timestamped. These index builds
    // are foregrounded and timestamp their catalog writes with a "commit timestamp". Nodes in the
    // oplog application phase of initial sync (`startup2`) must not timestamp index builds before
    // the `initialDataTimestamp`.
    const auto memberState = replCoord->getMemberState();
    if (memberState.startup() || memberState.startup2()) {
        return false;
    }

    return true;
}

// Synchronization tools when replication spawns a background index in a new thread.
// The bool is 'true' when a new background index has started in a new thread but the
// parent thread has not yet synchronized with it.
bool _bgIndexStarting(false);
stdx::mutex _bgIndexStartingMutex;
stdx::condition_variable _bgIndexStartingCondVar;

void _setBgIndexStarting() {
    stdx::lock_guard<stdx::mutex> lk(_bgIndexStartingMutex);
    invariant(_bgIndexStarting == false);
    _bgIndexStarting = true;
    _bgIndexStartingCondVar.notify_one();
}
}  // namespace

IndexBuilder::IndexBuilder(const BSONObj& index,
                           IndexConstraints indexConstraints,
                           ReplicatedWrites replicatedWrites,
                           Timestamp initIndexTs)
    : BackgroundJob(true /* self-delete */),
      _index(index.getOwned()),
      _indexConstraints(indexConstraints),
      _replicatedWrites(replicatedWrites),
      _initIndexTs(initIndexTs),
      _name(str::stream() << "repl index builder " << _indexBuildCount.addAndFetch(1)) {}

IndexBuilder::~IndexBuilder() {}

std::string IndexBuilder::name() const {
    return _name;
}

void IndexBuilder::run() {
    Client::initThread(name().c_str());
    ON_BLOCK_EXIT([] { Client::destroy(); });
    LOG(2) << "IndexBuilder building index " << _index;

    auto opCtx = cc().makeOperationContext();
    ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(opCtx->lockState());

    // If the calling thread is not replicating writes, neither should this thread.
    boost::optional<repl::UnreplicatedWritesBlock> unreplicatedWrites;
    if (_replicatedWrites == ReplicatedWrites::kUnreplicated) {
        unreplicatedWrites.emplace(opCtx.get());
    }

    AuthorizationSession::get(opCtx->getClient())->grantInternalAuthorization(opCtx.get());

    {
        stdx::lock_guard<Client> lk(*(opCtx->getClient()));
        CurOp::get(opCtx.get())->setNetworkOp_inlock(dbInsert);
    }
    NamespaceString ns(_index["ns"].String());

    Lock::DBLock dlk(opCtx.get(), ns.db(), MODE_X);
    OldClientContext ctx(opCtx.get(), ns.getSystemIndexesCollection());

    Database* db = DatabaseHolder::getDatabaseHolder().get(opCtx.get(), ns.db().toString());

    Status status = _build(opCtx.get(), db, true, &dlk);
    if (!status.isOK()) {
        error() << "IndexBuilder could not build index: " << redact(status);
        fassert(28555, ErrorCodes::isInterruption(status.code()));
    }
}

Status IndexBuilder::buildInForeground(OperationContext* opCtx, Database* db) const {
    return _build(opCtx, db, false, NULL);
}

void IndexBuilder::waitForBgIndexStarting() {
    stdx::unique_lock<stdx::mutex> lk(_bgIndexStartingMutex);
    while (_bgIndexStarting == false) {
        _bgIndexStartingCondVar.wait(lk);
    }
    // Reset for next time.
    _bgIndexStarting = false;
}

namespace {
/**
 * @param status shalt not be of code `WriteConflict`.
 */
Status _failIndexBuild(MultiIndexBlock& indexer, Status status, bool allowBackgroundBuilding) {
    invariant(status.code() != ErrorCodes::WriteConflict);

    if (status.code() == ErrorCodes::InterruptedAtShutdown) {
        // leave it as-if kill -9 happened. This will be handled on restart.
        invariant(allowBackgroundBuilding);  // Foreground builds aren't interrupted.
        indexer.abortWithoutCleanup();
        return status;
    }

    if (allowBackgroundBuilding) {
        error() << "Background index build failed. Status: " << redact(status);
        fassertFailed(50769);
    } else {
        return status;
    }
}
}  // namespace

Status IndexBuilder::_build(OperationContext* opCtx,
                            Database* db,
                            bool allowBackgroundBuilding,
                            Lock::DBLock* dbLock) const try {
    const NamespaceString ns(_index["ns"].String());

    Collection* coll = db->getCollection(opCtx, ns);
    // Collections should not be implicitly created by the index builder.
    fassert(40409, coll);

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        // Show which index we're building in the curop display.
        CurOp::get(opCtx)->setOpDescription_inlock(_index);
    }

    MultiIndexBlock indexer(opCtx, coll);
    indexer.allowInterruption();
    if (allowBackgroundBuilding)
        indexer.allowBackgroundBuilding();

    Status status = Status::OK();
    {
        TimestampBlock tsBlock(opCtx, _initIndexTs);
        status = writeConflictRetry(
            opCtx, "Init index build", ns.ns(), [&] { return indexer.init(_index).getStatus(); });
    }

    if (status == ErrorCodes::IndexAlreadyExists ||
        (status == ErrorCodes::IndexOptionsConflict &&
         _indexConstraints == IndexConstraints::kRelax)) {
        LOG(1) << "Ignoring indexing error: " << redact(status);
        if (allowBackgroundBuilding) {
            // Must set this in case anyone is waiting for this build.
            _setBgIndexStarting();
        }
        return Status::OK();
    }
    if (!status.isOK()) {
        return _failIndexBuild(indexer, status, allowBackgroundBuilding);
    }

    if (allowBackgroundBuilding) {
        _setBgIndexStarting();
        invariant(dbLock);
        dbLock->relockWithMode(MODE_IX);
    }

    {
        Lock::CollectionLock collLock(opCtx->lockState(), ns.ns(), MODE_IX);
        // WriteConflict exceptions and statuses are not expected to escape this method.
        status = indexer.insertAllDocumentsInCollection();
    }
    if (!status.isOK()) {
        if (allowBackgroundBuilding) {
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());
            dbLock->relockWithMode(MODE_X);
            if (status == ErrorCodes::InterruptedAtShutdown)
                return _failIndexBuild(indexer, status, allowBackgroundBuilding);
            opCtx->checkForInterrupt();
        }
        return _failIndexBuild(indexer, status, allowBackgroundBuilding);
    }

    if (allowBackgroundBuilding) {
        dbLock->relockWithMode(MODE_X);
    }
    writeConflictRetry(opCtx, "Commit index build", ns.ns(), [opCtx, coll, &indexer, &ns] {
        WriteUnitOfWork wunit(opCtx);
        indexer.commit([opCtx, coll, &ns](const BSONObj& indexSpec) {
            opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                opCtx, ns, *(coll->uuid()), indexSpec, false);
        });

        if (requiresGhostCommitTimestamp(opCtx, ns)) {
            auto status = opCtx->recoveryUnit()->setTimestamp(
                LogicalClock::get(opCtx)->getClusterTime().asTimestamp());
            if (status.code() == ErrorCodes::BadValue) {
                log() << "Temporarily could not timestamp the index build commit, retrying. "
                      << status.reason();
                throw WriteConflictException();
            }
            fassert(50701, status);
        }
        wunit.commit();
    });

    if (allowBackgroundBuilding) {
        dbLock->relockWithMode(MODE_X);
        Database* reloadDb = DatabaseHolder::getDatabaseHolder().get(opCtx, ns.db());
        fassert(28553, reloadDb);
        fassert(28554, reloadDb->getCollection(opCtx, ns));
    }

    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}
}
