
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

#include <list>
#include <set>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/session_catalog_migration_source.h"
#include "mongo/s/request_types/move_chunk_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class BSONArrayBuilder;
class BSONObjBuilder;
class Collection;
class Database;
class RecordId;

class MigrationChunkClonerSourceLegacy final : public MigrationChunkClonerSource {
    MONGO_DISALLOW_COPYING(MigrationChunkClonerSourceLegacy);

public:
    MigrationChunkClonerSourceLegacy(MoveChunkRequest request,
                                     const BSONObj& shardKeyPattern,
                                     ConnectionString donorConnStr,
                                     HostAndPort recipientHost);
    ~MigrationChunkClonerSourceLegacy();

    Status startClone(OperationContext* opCtx) override;

    Status awaitUntilCriticalSectionIsAppropriate(OperationContext* opCtx,
                                                  Milliseconds maxTimeToWait) override;

    StatusWith<BSONObj> commitClone(OperationContext* opCtx) override;

    void cancelClone(OperationContext* opCtx) override;

    bool isDocumentInMigratingChunk(const BSONObj& doc) override;

    void onInsertOp(OperationContext* opCtx,
                    const BSONObj& insertedDoc,
                    const repl::OpTime& opTime) override;

    void onUpdateOp(OperationContext* opCtx,
                    const BSONObj& updatedDoc,
                    const repl::OpTime& opTime,
                    const repl::OpTime& prePostImageOpTime) override;

    void onDeleteOp(OperationContext* opCtx,
                    const BSONObj& deletedDocId,
                    const repl::OpTime& opTime,
                    const repl::OpTime& preImageOpTime) override;

    // Legacy cloner specific functionality

    /**
     * Returns the migration session id associated with this cloner, so stale sessions can be
     * disambiguated.
     */
    const MigrationSessionId& getSessionId() const {
        return _sessionId;
    }

    /**
     * Returns the rollback ID recorded at the beginning of session migration. If the underlying
     * SessionCatalogMigrationSource does not exist, that means this node is running as a standalone
     * and doesn't support retryable writes, so we return boost::none.
     */
    boost::optional<int> getRollbackIdAtInit() const {
        if (_sessionCatalogSource) {
            return _sessionCatalogSource->getRollbackIdAtInit();
        }
        return boost::none;
    }

    /**
     * Called by the recipient shard. Used to estimate how many more bytes of clone data are
     * remaining in the chunk cloner.
     */
    uint64_t getCloneBatchBufferAllocationSize();

    /**
     * Called by the recipient shard. Populates the passed BSONArrayBuilder with a set of documents,
     * which are part of the initial clone sequence.
     *
     * Returns OK status on success. If there were documents returned in the result argument, this
     * method should be called more times until the result is empty. If it returns failure, it is
     * not safe to call more methods on this class other than cancelClone.
     *
     * This method will return early if too much time is spent fetching the documents in order to
     * give a chance to the caller to perform some form of yielding. It does not free or acquire any
     * locks on its own.
     *
     * NOTE: Must be called with the collection lock held in at least IS mode.
     */
    Status nextCloneBatch(OperationContext* opCtx,
                          Collection* collection,
                          BSONArrayBuilder* arrBuilder);

    /**
     * Called by the recipient shard. Transfers the accummulated local mods from source to
     * destination. Must not be called before all cloned objects have been fetched through calls to
     * nextCloneBatch.
     *
     * NOTE: Must be called with the collection lock held in at least IS mode.
     */
    Status nextModsBatch(OperationContext* opCtx, Database* db, BSONObjBuilder* builder);

    /**
     * Appends to 'arrBuilder' oplog entries which wrote to the currently migrated chunk and contain
     * session information.
     *
     * If this function returns a valid OpTime, this means that the oplog appended are not
     * guaranteed to be majority committed and the caller has to wait for the returned opTime to be
     * majority committed before returning them to the donor shard.
     *
     * If the underlying SessionCatalogMigrationSource does not exist, that means this node is
     * running as a standalone and doesn't support retryable writes, so we return boost::none.
     *
     * This waiting is necessary because session migration is only allowed to send out committed
     * entries, as opposed to chunk migration, which can send out uncommitted documents. With chunk
     * migration, the uncommitted documents will not be visibile until the end of the migration
     * commits, which means that if it fails, they won't be visible, whereas session oplog entries
     * take effect immediately since they are appended to the chain.
     */
    boost::optional<repl::OpTime> nextSessionMigrationBatch(OperationContext* opCtx,
                                                            BSONArrayBuilder* arrBuilder);

    /**
     * Returns a notification that can be used to wait for new oplog that needs to be migrated.
     * If the value in the notification returns true, it means that there are no more new batches
     * that needs to be fetched because the migration has already entered the critical section or
     * aborted.
     *
     * Returns nullptr if there is no session migration associated with this migration.
     */
    std::shared_ptr<Notification<bool>> getNotificationForNextSessionMigrationBatch();

private:
    friend class DeleteNotificationStage;
    friend class LogOpForShardingHandler;

    // Represents the states in which the cloner can be
    enum State { kNew, kCloning, kDone };

    /**
     * Idempotent method, which cleans up any previously initialized state. It is safe to be called
     * at any time, but no methods should be called after it.
     */
    void _cleanup(OperationContext* opCtx);

    /**
     * Synchronously invokes the recipient shard with the specified command and either returns the
     * command response (if succeeded) or the status, if the command failed.
     */
    StatusWith<BSONObj> _callRecipient(const BSONObj& cmdObj);

    /**
     * Get the disklocs that belong to the chunk migrated and sort them in _cloneLocs (to avoid
     * seeking disk later).
     *
     * Returns OK or any error status otherwise.
     */
    Status _storeCurrentLocs(OperationContext* opCtx);

    /**
     * Insert items from docIdList to a new array with the given fieldName in the given builder. If
     * explode is true, the inserted object will be the full version of the document. Note that
     * whenever an item from the docList is inserted to the array, it will also be removed from
     * docList.
     *
     * Should be holding the collection lock for ns if explode is true.
     */
    void _xfer(OperationContext* opCtx,
               Database* db,
               std::list<BSONObj>* docIdList,
               BSONObjBuilder* builder,
               const char* fieldName,
               long long* sizeAccumulator,
               bool explode);

    // The original move chunk request
    const MoveChunkRequest _args;

    // The shard key associated with the namespace
    const ShardKeyPattern _shardKeyPattern;

    // The migration session id
    const MigrationSessionId _sessionId;

    // The resolved connection string of the donor shard
    const ConnectionString _donorConnStr;

    // The resolved primary of the recipient shard
    const HostAndPort _recipientHost;

    // Registered deletion notifications plan executor, which will listen for document deletions
    // during the cloning stage
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _deleteNotifyExec;

    std::unique_ptr<SessionCatalogMigrationSource> _sessionCatalogSource;

    // Protects the entries below
    stdx::mutex _mutex;

    // The current state of the cloner
    State _state{kNew};

    // List of record ids that needs to be transferred (initial clone)
    std::set<RecordId> _cloneLocs;

    // The estimated average object size during the clone phase. Used for buffer size
    // pre-allocation (initial clone).
    uint64_t _averageObjectSizeForCloneLocs{0};

    // List of _id of documents that were modified that must be re-cloned (xfer mods)
    std::list<BSONObj> _reload;

    // List of _id of documents that were deleted during clone that should be deleted later (xfer
    // mods)
    std::list<BSONObj> _deleted;

    // Total bytes in _reload + _deleted (xfer mods)
    uint64_t _memoryUsed{0};
};

}  // namespace mongo
