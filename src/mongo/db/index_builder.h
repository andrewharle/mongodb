
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

#include "mongo/base/status.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/background.h"

namespace mongo {

class Collection;
class Database;
class OperationContext;

/**
 * A helper class for replication to use for building indexes.
 * In standalone mode, we use the client connection thread for building indexes in the
 * background. In replication mode, secondaries must spawn a new thread to build background
 * indexes, since there are no client connection threads to use for such purpose.  IndexBuilder
 * is a subclass of BackgroundJob to enable this use.
 * This class is also used for building indexes in the foreground on secondaries, for
 * code convenience.  buildInForeground() is directly called by the replication applier to
 * build an index in the foreground; the properties of BackgroundJob are not used for this use
 * case.
 * For background index builds, BackgroundJob::go() is called on the IndexBuilder instance,
 * which begins a new thread at this class's run() method.  After go() is called in the
 * parent thread, waitForBgIndexStarting() must be called by the same parent thread,
 * before any other thread calls go() on any other IndexBuilder instance.  This is
 * ensured by the replication system, since commands are effectively run single-threaded
 * by the replication applier.
 * The argument "constraints" specifies whether we should honor or ignore index constraints,
 * The ignoring of constraints is for replication due to idempotency reasons.
 * The argument "replicatedWrites" specifies whether or not this operation should replicate
 * oplog entries associated with this index build.
 * The argument "initIndexTs" specifies the timestamp to be used to make the initial catalog write.
 */
class IndexBuilder : public BackgroundJob {
public:
    /**
     * Indicates whether or not to ignore indexing errors.
     */
    enum class IndexConstraints { kEnforce, kRelax };

    /**
     * Indicates whether or not to replicate writes.
     */
    enum class ReplicatedWrites { kReplicated, kUnreplicated };

    IndexBuilder(const BSONObj& index,
                 IndexConstraints constraints,
                 ReplicatedWrites replicatedWrites,
                 Timestamp initIndexTs = Timestamp::min());
    virtual ~IndexBuilder();

    virtual void run();

    /**
     * name of the builder, not the index
     */
    virtual std::string name() const;

    Status buildInForeground(OperationContext* opCtx, Database* db) const;

    /**
     * Waits for a background index build to register itself.  This function must be called
     * after starting a background index build via a BackgroundJob and before starting a
     * subsequent one.
     */
    static void waitForBgIndexStarting();

private:
    Status _build(OperationContext* opCtx,
                  Database* db,
                  bool allowBackgroundBuilding,
                  Lock::DBLock* dbLock) const;

    const BSONObj _index;
    const IndexConstraints _indexConstraints;
    const ReplicatedWrites _replicatedWrites;
    const Timestamp _initIndexTs;
    std::string _name;  // name of this builder, not related to the index
    static AtomicUInt32 _indexBuildCount;
};
}
