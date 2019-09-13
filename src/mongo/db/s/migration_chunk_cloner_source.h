
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

#include "mongo/base/disallow_copying.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class OperationContext;
class Status;
class Timestamp;

namespace repl {
class OpTime;
}  // namespace repl

/**
 * This class is responsible for producing chunk documents to be moved from donor to a recipient
 * shard and its methods represent cloning stages. Its lifetime is owned and controlled by a single
 * migration source manager which registers it for notifications from the replication subsystem
 * before calling startClone.
 *
 * Unless explicitly indicated, the methods on this class are not thread-safe.
 *
 * The pattern of using this interface is such that one thread instantiates it and registers it so
 * it begins receiving notifications from the replication subsystem through the
 * on[insert/update/delete]Op methods. It is up to the creator to decide how these methods end up
 * being called, but currently this is done through the CollectionShardingState. The creator then
 * kicks off the cloning as soon as possible by calling startClone.
 */
class MigrationChunkClonerSource {
    MONGO_DISALLOW_COPYING(MigrationChunkClonerSource);

public:
    virtual ~MigrationChunkClonerSource();

    /**
     * Blocking method, which prepares the object for serving as a source for migrations and tells
     * the recipient shard to start cloning. Before calling this method, this chunk cloner must be
     * registered for notifications from the replication subsystem (not checked here).
     *
     * NOTE: Must be called without any locks and must succeed, before any other methods are called
     * (except for cancelClone and [insert/update/delete]Op).
     */
    virtual Status startClone(OperationContext* opCtx) = 0;

    /**
     * Blocking method, which uses some custom selected logic for deciding whether it is appropriate
     * for the donor shard to enter critical section.
     *
     * If it returns a successful status, the caller must as soon as possible stop writes (by
     * entering critical section). On failure it may return any error. Known errors are:
     *  ExceededTimeLimit - if the maxTimeToWait was exceeded
     *
     * NOTE: Must be called without any locks.
     */
    virtual Status awaitUntilCriticalSectionIsAppropriate(OperationContext* opCtx,
                                                          Milliseconds maxTimeToWait) = 0;

    /**
     * Tell the recipient shard to commit the documents it has cloned so far. Must be called only
     * when it has been ensured that there will be no more changes happening to documents on the
     * donor shard. If this is not observed, the recipient might miss changes and thus lose data.
     *
     * This must only be called once and no more methods on the cloner must be used afterwards
     * regardless of whether it succeeds or not.
     *
     * Returns statistics about the move. These are informational only and should not be
     * interpreted by the caller for any means other than reporting.
     *
     * NOTE: Must be called without any locks.
     */
    virtual StatusWith<BSONObj> commitClone(OperationContext* opCtx) = 0;

    /**
     * Tells the recipient to abort the clone and cleanup any unused data. This method's
     * implementation should be idempotent and never throw.
     *
     * NOTE: Must be called without any locks.
     */
    virtual void cancelClone(OperationContext* opCtx) = 0;

    // These methods are only meaningful for the legacy cloner and they are used as a way to keep a
    // running list of changes, which need to be fetched.

    /**
     * Checks whether the specified document is within the bounds of the chunk, which this cloner
     * is responsible for.
     *
     * NOTE: Must be called with at least IS lock held on the collection.
     */
    virtual bool isDocumentInMigratingChunk(const BSONObj& doc) = 0;

    /**
     * Notifies this cloner that an insert happened to the collection, which it owns. It is up to
     * the cloner's implementation to decide what to do with this information and it is valid for
     * the implementation to ignore it.
     *
     * NOTE: Must be called with at least IX lock held on the collection.
     */
    virtual void onInsertOp(OperationContext* opCtx,
                            const BSONObj& insertedDoc,
                            const repl::OpTime& opTime) = 0;

    /**
     * Notifies this cloner that an update happened to the collection, which it owns. It is up to
     * the cloner's implementation to decide what to do with this information and it is valid for
     * the implementation to ignore it.
     *
     * NOTE: Must be called with at least IX lock held on the collection.
     */
    virtual void onUpdateOp(OperationContext* opCtx,
                            const BSONObj& updatedDoc,
                            const repl::OpTime& opTime,
                            const repl::OpTime& prePostImageOpTime) = 0;

    /**
     * Notifies this cloner that a delede happened to the collection, which it owns. It is up to the
     * cloner's implementation to decide what to do with this information and it is valid for the
     * implementation to ignore it.
     *
     * NOTE: Must be called with at least IX lock held on the collection.
     */
    virtual void onDeleteOp(OperationContext* opCtx,
                            const BSONObj& deletedDocId,
                            const repl::OpTime& opTime,
                            const repl::OpTime& preImageOpTime) = 0;

protected:
    MigrationChunkClonerSource();
};

}  // namespace mongo
