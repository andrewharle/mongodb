/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/commands/write_commands/batch_executor.h"

#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/ops/delete_executor.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/update_executor.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_server_status.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/write_ops/batched_upsert_detail.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    namespace {

        /**
         * Data structure to safely hold and clean up results of single write operations.
         */
        class WriteOpResult {
            MONGO_DISALLOW_COPYING(WriteOpResult);
        public:
            WriteOpResult() {}

            WriteOpStats& getStats() { return _stats; }

            WriteErrorDetail* getError() { return _error.get(); }
            WriteErrorDetail* releaseError() { return _error.release(); }
            void setError(WriteErrorDetail* error) { _error.reset(error); }

        private:
            WriteOpStats _stats;
            std::auto_ptr<WriteErrorDetail> _error;
        };

        /**
	 * Stores the shard version of a namespace on creation and restores it
	 * back on destruction if the version was changed to ignored.
	 */
        class UndoShardVersionIgnore {
        public:
	    UndoShardVersionIgnore(const std::string& ns, ShardedConnectionInfo* info)
	        : _ns(ns), _info(info) {
	        if (_info) {
	            _originalVersion = _info->getVersion(_ns);
            }
        }

        ~UndoShardVersionIgnore() {
            if (_info && ChunkVersion::isIgnoredVersion(_info->getVersion(_ns))) {
                _info->setVersion(_ns, _originalVersion);
            }
        }

        private:
	    std::string _ns;
	    ChunkVersion _originalVersion;
	    ShardedConnectionInfo* _info;
	};

    }  // namespace

    // TODO: Determine queueing behavior we want here
    MONGO_EXPORT_SERVER_PARAMETER( queueForMigrationCommit, bool, true );

    using mongoutils::str::stream;

    WriteBatchExecutor::WriteBatchExecutor( const BSONObj& wc,
                                            Client* client,
                                            OpCounters* opCounters,
                                            LastError* le ) :
        _defaultWriteConcern( wc ),
        _client( client ),
        _opCounters( opCounters ),
        _le( le ),
        _stats( new WriteBatchStats ) {
    }

    static WCErrorDetail* toWriteConcernError( const Status& wcStatus,
                                               const WriteConcernResult& wcResult ) {

        WCErrorDetail* wcError = new WCErrorDetail;

        wcError->setErrCode( wcStatus.code() );
        wcError->setErrMessage( wcStatus.reason() );
        if ( wcResult.wTimedOut )
            wcError->setErrInfo( BSON( "wtimeout" << true ) );

        return wcError;
    }

    static WriteErrorDetail* toWriteError( const Status& status ) {

        WriteErrorDetail* error = new WriteErrorDetail;

        // TODO: Complex transform here?
        error->setErrCode( status.code() );
        error->setErrMessage( status.reason() );

        return error;
    }

    static void toBatchError( const Status& status, BatchedCommandResponse* response ) {
        response->clear();
        response->setErrCode( status.code() );
        response->setErrMessage( status.reason() );
        response->setOk( false );
        dassert( response->isValid(NULL) );
    }

    static void noteInCriticalSection( WriteErrorDetail* staleError ) {
        BSONObjBuilder builder;
        if ( staleError->isErrInfoSet() )
            builder.appendElements( staleError->getErrInfo() );
        builder.append( "inCriticalSection", true );
        staleError->setErrInfo( builder.obj() );
    }

    void WriteBatchExecutor::executeBatch( const BatchedCommandRequest& request,
                                           BatchedCommandResponse* response ) {

        // Validate namespace
        const NamespaceString nss = NamespaceString( request.getNS() );
        if ( !nss.isValid() ) {
            toBatchError( Status( ErrorCodes::InvalidNamespace,
                                  nss.ns() + " is not a valid namespace" ),
                          response );
            return;
        }

        // Make sure we can write to the namespace
        Status allowedStatus = userAllowedWriteNS( nss );
        if ( !allowedStatus.isOK() ) {
            toBatchError( allowedStatus, response );
            return;
        }

        // Validate insert index requests
        // TODO: Push insert index requests through createIndex once all upgrade paths support it
        string errMsg;
        if ( request.isInsertIndexRequest() && !request.isValidIndexRequest( &errMsg ) ) {
            toBatchError( Status( ErrorCodes::InvalidOptions, errMsg ), response );
            return;
        }

        // Validate write concern
        // TODO: Lift write concern parsing out of this entirely
        WriteConcernOptions writeConcern;

        BSONObj wcDoc;
        if ( request.isWriteConcernSet() ) {
            wcDoc = request.getWriteConcern();
        }

        Status wcStatus = Status::OK();
        if ( wcDoc.isEmpty() ) {
            // The default write concern if empty is w : 1
            // Specifying w : 0 is/was allowed, but is interpreted identically to w : 1

            wcStatus = writeConcern.parse(
                _defaultWriteConcern.isEmpty() ?
                    WriteConcernOptions::Acknowledged : _defaultWriteConcern );

            if ( writeConcern.wNumNodes == 0 && writeConcern.wMode.empty() ) {
                writeConcern.wNumNodes = 1;
            }
        }
        else {
            wcStatus = writeConcern.parse( wcDoc );
        }

        if ( wcStatus.isOK() ) {
            wcStatus = validateWriteConcern( writeConcern );
        }

        if ( !wcStatus.isOK() ) {
            toBatchError( wcStatus, response );
            return;
        }

        if ( request.sizeWriteOps() == 0u ) {
            toBatchError( Status( ErrorCodes::InvalidLength,
                                  "no write ops were included in the batch" ),
                          response );
            return;
        }

        // Validate batch size
        if ( request.sizeWriteOps() > BatchedCommandRequest::kMaxWriteBatchSize ) {
            toBatchError( Status( ErrorCodes::InvalidLength,
                                  stream() << "exceeded maximum write batch size of "
                                           << BatchedCommandRequest::kMaxWriteBatchSize ),
                          response );
            return;
        }

        //
        // End validation
        //

        bool silentWC = writeConcern.wMode.empty() && writeConcern.wNumNodes == 0
                        && writeConcern.syncMode == WriteConcernOptions::NONE;

        Timer commandTimer;

        OwnedPointerVector<WriteErrorDetail> writeErrorsOwned;
        vector<WriteErrorDetail*>& writeErrors = writeErrorsOwned.mutableVector();

        OwnedPointerVector<BatchedUpsertDetail> upsertedOwned;
        vector<BatchedUpsertDetail*>& upserted = upsertedOwned.mutableVector();

	UndoShardVersionIgnore undoShardVersionIgnore(request.getTargetingNS(),
                                                      ShardedConnectionInfo::get(false));

        //
        // Apply each batch item, possibly bulking some items together in the write lock.
        // Stops on error if batch is ordered.
        //

        bulkExecute( request, &upserted, &writeErrors );

        //
        // Try to enforce the write concern if everything succeeded (unordered or ordered)
        // OR if something succeeded and we're unordered.
        //

        auto_ptr<WCErrorDetail> wcError;
        bool needToEnforceWC = writeErrors.empty()
                               || ( !request.getOrdered()
                                    && writeErrors.size() < request.sizeWriteOps() );

        if ( needToEnforceWC ) {

            _client->curop()->setMessage( "waiting for write concern" );

            WriteConcernResult res;
            Status status = waitForWriteConcern( writeConcern, _client->getLastOp(), &res );

            if ( !status.isOK() ) {
                wcError.reset( toWriteConcernError( status, res ) );
            }
        }

        //
        // Refresh metadata if needed
        //

        bool staleBatch = !writeErrors.empty()
                          && writeErrors.back()->getErrCode() == ErrorCodes::StaleShardVersion;

        if ( staleBatch ) {

            const BatchedRequestMetadata* requestMetadata = request.getMetadata();
            dassert( requestMetadata );

            // Make sure our shard name is set or is the same as what was set previously
            if ( shardingState.setShardName( requestMetadata->getShardName() ) ) {

                //
                // First, we refresh metadata if we need to based on the requested version.
                //

                ChunkVersion latestShardVersion;
                shardingState.refreshMetadataIfNeeded( request.getTargetingNS(),
                                                       requestMetadata->getShardVersion(),
                                                       &latestShardVersion );

                // Report if we're still changing our metadata
                // TODO: Better reporting per-collection
                if ( shardingState.inCriticalMigrateSection() ) {
                    noteInCriticalSection( writeErrors.back() );
                }

                if ( queueForMigrationCommit ) {

                    //
                    // Queue up for migration to end - this allows us to be sure that clients will
                    // not repeatedly try to refresh metadata that is not yet written to the config
                    // server.  Not necessary for correctness.
                    // Exposed as optional parameter to allow testing of queuing behavior with
                    // different network timings.
                    //

                    const ChunkVersion& requestShardVersion = requestMetadata->getShardVersion();

                    //
                    // Only wait if we're an older version (in the current collection epoch) and
                    // we're not write compatible, implying that the current migration is affecting
                    // writes.
                    //

                    if ( requestShardVersion.isOlderThan( latestShardVersion ) &&
                         !requestShardVersion.isWriteCompatibleWith( latestShardVersion ) ) {

                        while ( shardingState.inCriticalMigrateSection() ) {

                            log() << "write request to old shard version "
                                  << requestMetadata->getShardVersion().toString()
                                  << " waiting for migration commit" << endl;

                            shardingState.waitTillNotInCriticalSection( 10 /* secs */);
                        }
                    }
                }
            }
            else {
                // If our shard name is stale, our version must have been stale as well
                dassert( writeErrors.size() == request.sizeWriteOps() );
            }
        }

        //
        // Construct response
        //

        response->setOk( true );

        if ( !silentWC ) {

            if ( upserted.size() ) {
                response->setUpsertDetails( upserted );
            }

            if ( writeErrors.size() ) {
                response->setErrDetails( writeErrors );
            }

            if ( wcError.get() ) {
                response->setWriteConcernError( wcError.release() );
            }

            if ( anyReplEnabled() ) {
                response->setLastOp( _client->getLastOp() );
                if (theReplSet) {
                    response->setElectionId( theReplSet->getElectionId() );
                }
            }

            // Set the stats for the response
            response->setN( _stats->numInserted + _stats->numUpserted + _stats->numMatched
                            + _stats->numDeleted );
            if ( request.getBatchType() == BatchedCommandRequest::BatchType_Update )
                response->setNModified( _stats->numModified );
        }

        dassert( response->isValid( NULL ) );
    }

    // Translates write item type to wire protocol op code.
    // Helper for WriteBatchExecutor::applyWriteItem().
    static int getOpCode( BatchedCommandRequest::BatchType writeType ) {
        switch ( writeType ) {
        case BatchedCommandRequest::BatchType_Insert:
            return dbInsert;
        case BatchedCommandRequest::BatchType_Update:
            return dbUpdate;
        default:
            dassert( writeType == BatchedCommandRequest::BatchType_Delete );
            return dbDelete;
        }
        return 0;
    }

    static void buildStaleError( const ChunkVersion& shardVersionRecvd,
                                 const ChunkVersion& shardVersionWanted,
                                 WriteErrorDetail* error ) {

        // Write stale error to results
        error->setErrCode( ErrorCodes::StaleShardVersion );

        BSONObjBuilder infoB;
        shardVersionWanted.addToBSON( infoB, "vWanted" );
        error->setErrInfo( infoB.obj() );

        string errMsg = stream() << "stale shard version detected before write, received "
                                 << shardVersionRecvd.toString() << " but local version is "
                                 << shardVersionWanted.toString();
        error->setErrMessage( errMsg );
    }

    static bool checkShardVersion(ShardingState* shardingState,
                                  const BatchedCommandRequest& request,
                                  WriteOpResult* result) {

        const NamespaceString nss( request.getTargetingNS() );
        Lock::assertWriteLocked( nss.ns() );

        ChunkVersion requestShardVersion =
            request.isMetadataSet() && request.getMetadata()->isShardVersionSet() ?
                request.getMetadata()->getShardVersion() : ChunkVersion::IGNORED();

        if ( shardingState->enabled() ) {

            CollectionMetadataPtr metadata = shardingState->getCollectionMetadata( nss.ns() );

            if ( !ChunkVersion::isIgnoredVersion( requestShardVersion ) ) {

                ChunkVersion shardVersion =
                    metadata ? metadata->getShardVersion() : ChunkVersion::UNSHARDED();

                if ( !requestShardVersion.isWriteCompatibleWith( shardVersion ) ) {
                    result->setError(new WriteErrorDetail);
                    buildStaleError(requestShardVersion, shardVersion, result->getError());
                    return false;
                }
            }
        }

        return true;
    }

    static bool checkIsMasterForCollection(const std::string& ns, WriteOpResult* result) {
        if (!isMasterNs(ns.c_str())) {
            WriteErrorDetail* errorDetail = new WriteErrorDetail;
            result->setError(errorDetail);
            errorDetail->setErrCode(ErrorCodes::NotMaster);
            errorDetail->setErrMessage("Not primary while writing to " + ns);
            return false;
        }
        return true;
    }

    static void buildUniqueIndexError( const BSONObj& keyPattern,
                                       const BSONObj& indexPattern,
                                       WriteErrorDetail* error ) {
        error->setErrCode( ErrorCodes::CannotCreateIndex );
        string errMsg = stream() << "cannot create unique index over " << indexPattern
                                 << " with shard key pattern " << keyPattern;
        error->setErrMessage( errMsg );
    }

    static bool checkIndexConstraints(ShardingState* shardingState,
                                      const BatchedCommandRequest& request,
                                      WriteOpResult* result) {

        const NamespaceString nss( request.getTargetingNS() );
        Lock::assertWriteLocked( nss.ns() );

        if ( !request.isUniqueIndexRequest() )
            return true;

        if ( shardingState->enabled() ) {

            CollectionMetadataPtr metadata = shardingState->getCollectionMetadata( nss.ns() );

            if ( metadata ) {
                if ( !isUniqueIndexCompatible( metadata->getKeyPattern(),
                                               request.getIndexKeyPattern() ) ) {

                    result->setError(new WriteErrorDetail);
                    buildUniqueIndexError(metadata->getKeyPattern(),
                                          request.getIndexKeyPattern(),
                                          result->getError());

                    return false;
                }
            }
        }

        return true;
    }

    //
    // HELPERS FOR CUROP MANAGEMENT AND GLOBAL STATS
    //

    static CurOp* beginCurrentOp( Client* client, const BatchItemRef& currWrite ) {

        // Execute the write item as a child operation of the current operation.
        auto_ptr<CurOp> currentOp( new CurOp( client, client->curop() ) );

        // Set up the child op with more info
        HostAndPort remote =
            client->hasRemote() ? client->getRemote() : HostAndPort( "0.0.0.0", 0 );
        // TODO Modify CurOp "wrapped" constructor to take an opcode, so calling .reset()
        // is unneeded
        currentOp->reset( remote, getOpCode( currWrite.getRequest()->getBatchType() ) );
        currentOp->ensureStarted();
        currentOp->setNS( currWrite.getRequest()->getNS() );

        currentOp->debug().ns = currentOp->getNS();
        currentOp->debug().op = currentOp->getOp();

        if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Insert ) {
            currentOp->setQuery( currWrite.getDocument() );
            currentOp->debug().query = currWrite.getDocument();
            currentOp->debug().ninserted = 0;
        }
        else if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Update ) {
            currentOp->setQuery( currWrite.getUpdate()->getQuery() );
            currentOp->debug().query = currWrite.getUpdate()->getQuery();
            currentOp->debug().updateobj = currWrite.getUpdate()->getUpdateExpr();
            // Note: debug().nMatched, nModified and nmoved are set internally in update
        }
        else {
            dassert( currWrite.getOpType() == BatchedCommandRequest::BatchType_Delete );
            currentOp->setQuery( currWrite.getDelete()->getQuery() );
            currentOp->debug().query = currWrite.getDelete()->getQuery();
            currentOp->debug().ndeleted = 0;
        }

        return currentOp.release();
    }

    void WriteBatchExecutor::incOpStats( const BatchItemRef& currWrite ) {

        if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Insert ) {
            _opCounters->gotInsert();
        }
        else if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Update ) {
            _opCounters->gotUpdate();
        }
        else {
            dassert( currWrite.getOpType() == BatchedCommandRequest::BatchType_Delete );
            _opCounters->gotDelete();
        }
    }

    void WriteBatchExecutor::incWriteStats( const BatchItemRef& currWrite,
                                            const WriteOpStats& stats,
                                            const WriteErrorDetail* error,
                                            CurOp* currentOp ) {

        if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Insert ) {
            _stats->numInserted += stats.n;
            _le->nObjects = stats.n;
            currentOp->debug().ninserted += stats.n;
        }
        else if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Update ) {
            if ( stats.upsertedID.isEmpty() ) {
                _stats->numMatched += stats.n;
                _stats->numModified += stats.nModified;
            }
            else {
                ++_stats->numUpserted;
            }

            if ( !error ) {
                _le->recordUpdate( stats.upsertedID.isEmpty() && stats.n > 0,
                        stats.n,
                        stats.upsertedID );
            }
        }
        else {
            dassert( currWrite.getOpType() == BatchedCommandRequest::BatchType_Delete );
            _stats->numDeleted += stats.n;
            if ( !error ) {
                _le->recordDelete( stats.n );
            }
            currentOp->debug().ndeleted += stats.n;
        }

        if (error && !_le->disabled) {
            _le->raiseError(error->getErrCode(), error->getErrMessage().c_str());
        }
    }

    static void finishCurrentOp( Client* client, CurOp* currentOp, WriteErrorDetail* opError ) {

        currentOp->done();
        int executionTime = currentOp->debug().executionTime = currentOp->totalTimeMillis();
        currentOp->debug().recordStats();

        if ( opError ) {
            currentOp->debug().exceptionInfo = ExceptionInfo( opError->getErrMessage(),
                                                              opError->getErrCode() );

            MONGO_TLOG(3) << " Caught Assertion in " << opToString( currentOp->getOp() )
                          << ", continuing " << causedBy( opError->getErrMessage() ) << endl;
        }

        bool logAll = logger::globalLogDomain()->shouldLog( logger::LogSeverity::Debug( 1 ) );
        bool logSlow = executionTime
                       > ( serverGlobalParams.slowMS + currentOp->getExpectedLatencyMs() );

        if ( logAll || logSlow ) {
            MONGO_TLOG(0) << currentOp->debug().report( *currentOp ) << endl;
        }

        if ( currentOp->shouldDBProfile( executionTime ) ) {
            profile( *client, currentOp->getOp(), *currentOp );
        }
    }

    // END HELPERS

    //
    // CORE WRITE OPERATIONS (declaration)
    // These functions write to the database and return stats and zero or one of:
    // - page fault
    // - error
    //

    static void singleInsert( const BSONObj& docToInsert,
                              Collection* collection,
                              const PregeneratedKeys* pregen,
                              WriteOpResult* result );

    static void singleCreateIndex( const BSONObj& indexDesc,
                                   Collection* collection,
                                   WriteOpResult* result );

    static void multiUpdate( const BatchItemRef& updateItem,
                             WriteOpResult* result );

    static void multiRemove( const BatchItemRef& removeItem, WriteOpResult* result );

    //
    // WRITE EXECUTION
    // In general, the exec* operations manage db lock state and stats before dispatching to the
    // core write operations, which are *only* responsible for performing a write and reporting
    // success or failure.
    //

    /**
     * Representation of the execution state of execInserts.  Used by a single
     * execution of execInserts in a single thread.
     */
    class WriteBatchExecutor::ExecInsertsState {
        MONGO_DISALLOW_COPYING(ExecInsertsState);
    public:
        /**
         * Constructs a new instance, for performing inserts described in "aRequest".
         */
        explicit ExecInsertsState(const BatchedCommandRequest* aRequest);

        /**
         * Acquires the write lock and client context needed to perform the current write operation.
         * Returns true on success, after which it is safe to use the "context" and "collection"
         * members.  It is safe to call this function if this instance already holds the write lock.
         *
         * On failure, writeLock, context and collection will be NULL/clear.
         */
        bool lockAndCheck(WriteOpResult* result);

        /**
         * Releases the client context and write lock acquired by lockAndCheck.  Safe to call
         * regardless of whether or not this state object currently owns the lock.
         */
        void unlock();

        /**
         * Returns true if this executor has the lock on the target database.
         */
        bool hasLock() { return _writeLock.get(); }

        /**
         * Gets the lock-holding object.  Only valid if hasLock().
         */
        Lock::DBWrite& getLock() { return *_writeLock; }

        /**
         * Gets the target collection for the batch operation.  Value is undefined
         * unless hasLock() is true.
         */
        Collection* getCollection() { return _collection; }

        // Request object describing the inserts.
        const BatchedCommandRequest* request;

        // Index of the current insert operation to perform.
        size_t currIndex;

        // Translation of insert documents in "request" into insert-ready forms.  This vector has a
        // correspondence with elements of the "request", and "currIndex" is used to
        // index both.
        std::vector<StatusWith<BSONObj> > normalizedInserts;

        // We generate these outside of any locks
        std::vector<PregeneratedKeys> pregeneratedKeys;

    private:
        bool _lockAndCheckImpl(WriteOpResult* result);

        // Guard object for the write lock on the target database.
        scoped_ptr<Lock::DBWrite> _writeLock;

        // Context object on the target database.  Must appear after writeLock, so that it is
        // destroyed in proper order.
        scoped_ptr<Client::Context> _context;

        // Target collection.
        Collection* _collection;
    };

    void WriteBatchExecutor::bulkExecute( const BatchedCommandRequest& request,
                                          std::vector<BatchedUpsertDetail*>* upsertedIds,
                                          std::vector<WriteErrorDetail*>* errors ) {

        if ( request.getBatchType() == BatchedCommandRequest::BatchType_Insert ) {
            execInserts( request, errors );
        }
        else if ( request.getBatchType() == BatchedCommandRequest::BatchType_Update ) {
            for ( size_t i = 0; i < request.sizeWriteOps(); i++ ) {

                WriteErrorDetail* error = NULL;
                BSONObj upsertedId;
                execUpdate( BatchItemRef( &request, i ), &upsertedId, &error );

                if ( !upsertedId.isEmpty() ) {
                    BatchedUpsertDetail* batchUpsertedId = new BatchedUpsertDetail;
                    batchUpsertedId->setIndex( i );
                    batchUpsertedId->setUpsertedID( upsertedId );
                    upsertedIds->push_back( batchUpsertedId );
                }

                if ( error ) {
                    errors->push_back( error );
                    if ( request.getOrdered() )
                        break;
                }
            }
        }
        else {
            dassert( request.getBatchType() == BatchedCommandRequest::BatchType_Delete );
            for ( size_t i = 0; i < request.sizeWriteOps(); i++ ) {

                WriteErrorDetail* error = NULL;
                execRemove( BatchItemRef( &request, i ), &error );

                if ( error ) {
                    errors->push_back( error );
                    if ( request.getOrdered() )
                        break;
                }
            }
        }

        // Fill in stale version errors for unordered batches (update/delete can't do this on own)
        if ( !errors->empty() && !request.getOrdered() ) {

            const WriteErrorDetail* finalError = errors->back();

            if ( finalError->getErrCode() == ErrorCodes::StaleShardVersion ) {
                for ( size_t i = finalError->getIndex() + 1; i < request.sizeWriteOps(); i++ ) {
                    WriteErrorDetail* dupStaleError = new WriteErrorDetail;
                    finalError->cloneTo( dupStaleError );
                    errors->push_back( dupStaleError );
                }
            }
        }
    }

    // Goes over the request and preprocesses normalized versions of all the inserts in the request
    static void normalizeInserts( const BatchedCommandRequest& request,
                                  vector<StatusWith<BSONObj> >* normalizedInserts,
                                  vector<PregeneratedKeys>* pregen ) {

        normalizedInserts->reserve(request.sizeWriteOps());
        for ( size_t i = 0; i < request.sizeWriteOps(); ++i ) {
            BSONObj insertDoc = request.getInsertRequest()->getDocumentsAt( i );
            StatusWith<BSONObj> normalInsert = fixDocumentForInsert( insertDoc );
            normalizedInserts->push_back( normalInsert );
            if ( request.getOrdered() && !normalInsert.isOK() )
                break;

            if ( !normalInsert.getValue().isEmpty() )
                insertDoc = normalInsert.getValue();

            pregen->push_back( PregeneratedKeys() );
            GeneratorHolder::getInstance()->prepare( request.getTargetingNS(),
                                                     insertDoc,
                                                     &pregen->back() );
        }
    }

    void WriteBatchExecutor::execInserts( const BatchedCommandRequest& request,
                                          std::vector<WriteErrorDetail*>* errors ) {

        // Theory of operation:
        //
        // Instantiates an ExecInsertsState, which represents all of the state involved in the batch
        // insert execution algorithm.  Most importantly, encapsulates the lock state.
        //
        // Every iteration of the loop in execInserts() processes one document insertion, by calling
        // insertOne() exactly once for a given value of state.currIndex.
        //
        // If the ExecInsertsState indicates that the requisite write locks are not held, insertOne
        // acquires them and performs lock-acquisition-time checks.  However, on non-error
        // execution, it does not release the locks.  Therefore, the yielding logic in the while
        // loop in execInserts() is solely responsible for lock release in the non-error case.
        //
        // Internally, insertOne loops performing the single insert until it completes without a
        // PageFaultException, or until it fails with some kind of error.  Errors are mostly
        // propagated via the request->error field, but DBExceptions or std::exceptions may escape,
        // particularly on operation interruption.  These kinds of errors necessarily prevent
        // further insertOne calls, and stop the batch.  As a result, the only expected source of
        // such exceptions are interruptions.
        ExecInsertsState state(&request);
        normalizeInserts(request, &state.normalizedInserts, &state.pregeneratedKeys);

        ElapsedTracker elapsedTracker(128, 10); // 128 hits or 10 ms, matching RunnerYieldPolicy's

        ShardedConnectionInfo* info = ShardedConnectionInfo::get(false);
        if (info) {
            if (request.isMetadataSet() && request.getMetadata()->isShardVersionSet()) {
                info->setVersion(request.getTargetingNS(),
                                 request.getMetadata()->getShardVersion());
            }
            else {
                info->setVersion(request.getTargetingNS(), ChunkVersion::IGNORED());
            }
        }

        for (state.currIndex = 0;
             state.currIndex < state.request->sizeWriteOps();
             ++state.currIndex) {

            if (elapsedTracker.intervalHasElapsed()) {
                // Consider yielding between inserts.

                if (state.hasLock()) {
                    int micros = ClientCursor::suggestYieldMicros();
                    if (micros > 0) {
                        state.unlock();
                        killCurrentOp.checkForInterrupt();
                        sleepmicros(micros);
                    }
                }
                killCurrentOp.checkForInterrupt();
                elapsedTracker.resetLastTime();
            }

            WriteErrorDetail* error = NULL;
            execOneInsert(&state, &error);
            if (error) {
                errors->push_back(error);
                error->setIndex(state.currIndex);
                if (request.getOrdered())
                    return;
            }
        }
    }

    void WriteBatchExecutor::execUpdate( const BatchItemRef& updateItem,
                                         BSONObj* upsertedId,
                                         WriteErrorDetail** error ) {

        // BEGIN CURRENT OP
        scoped_ptr<CurOp> currentOp( beginCurrentOp( _client, updateItem ) );
        incOpStats( updateItem );

        ShardedConnectionInfo* info = ShardedConnectionInfo::get(false);
        if (info) {
            const BatchedCommandRequest* rootRequest = updateItem.getRequest();
            if (!updateItem.getUpdate()->getMulti() &&
                    rootRequest->isMetadataSet() &&
                    rootRequest->getMetadata()->isShardVersionSet()) {
                info->setVersion(rootRequest->getTargetingNS(),
                                 rootRequest->getMetadata()->getShardVersion());
            }
            else {
                info->setVersion(rootRequest->getTargetingNS(), ChunkVersion::IGNORED());
            }
        }

        WriteOpResult result;
        multiUpdate( updateItem, &result );

        if ( !result.getStats().upsertedID.isEmpty() ) {
            *upsertedId = result.getStats().upsertedID;
        }

        // END CURRENT OP
        incWriteStats( updateItem, result.getStats(), result.getError(), currentOp.get() );
        finishCurrentOp( _client, currentOp.get(), result.getError() );

        if ( result.getError() ) {
            result.getError()->setIndex( updateItem.getItemIndex() );
            *error = result.releaseError();
        }
    }

    void WriteBatchExecutor::execRemove( const BatchItemRef& removeItem,
                                         WriteErrorDetail** error ) {

        // Removes are similar to updates, but page faults are handled externally

        // BEGIN CURRENT OP
        scoped_ptr<CurOp> currentOp( beginCurrentOp( _client, removeItem ) );
        incOpStats( removeItem );

        ShardedConnectionInfo* info = ShardedConnectionInfo::get(false);
        if (info) {
            const BatchedCommandRequest* rootRequest = removeItem.getRequest();
            if (removeItem.getDelete()->getLimit() == 1 &&
                    rootRequest->isMetadataSet() &&
                    rootRequest->getMetadata()->isShardVersionSet()) {
                info->setVersion(rootRequest->getTargetingNS(),
                                 rootRequest->getMetadata()->getShardVersion());
            }
            else {
                info->setVersion(rootRequest->getTargetingNS(), ChunkVersion::IGNORED());
            }
        }

        WriteOpResult result;

        // NOTE: Deletes will not fault outside the lock once any data has been written
        {
            PageFaultRetryableSection pageFaultSection;
            while ( true ) {
                try {
                    multiRemove( removeItem, &result );
                    break;
                }
                catch (PageFaultException& pfe) {
                    pfe.touch();
                    invariant(!result.getError());
                    continue;
                }
                fassertFailed(17429);
            }
        }

        // END CURRENT OP
        incWriteStats( removeItem, result.getStats(), result.getError(), currentOp.get() );
        finishCurrentOp( _client, currentOp.get(), result.getError() );

        if ( result.getError() ) {
            result.getError()->setIndex( removeItem.getItemIndex() );
            *error = result.releaseError();
        }
    }

    //
    // IN-DB-LOCK CORE OPERATIONS
    //

    WriteBatchExecutor::ExecInsertsState::ExecInsertsState(const BatchedCommandRequest* aRequest) :
        request(aRequest),
        currIndex(0),
        _collection(NULL) {
    }

    bool WriteBatchExecutor::ExecInsertsState::_lockAndCheckImpl(WriteOpResult* result) {
        if (hasLock()) {
            cc().curop()->enter(_context.get());
            return true;
        }

        invariant(!_context.get());
        _writeLock.reset(new Lock::DBWrite(request->getNS()));
        if (!checkIsMasterForCollection(request->getNS(), result)) {
            return false;
        }
        if (!checkShardVersion(&shardingState, *request, result)) {
            return false;
        }
        if (!checkIndexConstraints(&shardingState, *request, result)) {
            return false;
        }
        _context.reset(new Client::Context(request->getNS(),
                                           storageGlobalParams.dbpath));
        Database* database = _context->db();
        dassert(database);
        _collection = database->getCollection(request->getTargetingNS());
        if (!_collection) {
            // Implicitly create if it doesn't exist
            _collection = database->createCollection(request->getTargetingNS());
            if (!_collection) {
                result->setError(
                        toWriteError(Status(ErrorCodes::InternalError,
                                            "could not create collection " +
                                            request->getTargetingNS())));
                return false;
            }
        }
        return true;
    }

    bool WriteBatchExecutor::ExecInsertsState::lockAndCheck(WriteOpResult* result) {
        if (_lockAndCheckImpl(result))
            return true;
        unlock();
        return false;
    }

    void WriteBatchExecutor::ExecInsertsState::unlock() {
        _collection = NULL;
        _context.reset();
        _writeLock.reset();
    }

    static void insertOne(WriteBatchExecutor::ExecInsertsState* state, WriteOpResult* result) {
        invariant(state->currIndex < state->normalizedInserts.size());
        const StatusWith<BSONObj>& normalizedInsert(state->normalizedInserts[state->currIndex]);

        if (!normalizedInsert.isOK()) {
            result->setError(toWriteError(normalizedInsert.getStatus()));
            return;
        }

        const BSONObj& insertDoc = normalizedInsert.getValue().isEmpty() ?
            state->request->getInsertRequest()->getDocumentsAt( state->currIndex ) :
            normalizedInsert.getValue();

        cc().clearHasWrittenThisOperation();
        {
            PageFaultRetryableSection pageFaultSection;
            while (true) {
                try {
                    if (!state->lockAndCheck(result)) {
                        break;
                    }

                    if (!state->request->isInsertIndexRequest()) {
                        const PregeneratedKeys* pregen = NULL;
                        if ( state->pregeneratedKeys.size() > state->currIndex )
                            pregen = &state->pregeneratedKeys[state->currIndex];
                        singleInsert(insertDoc, state->getCollection(), pregen, result);
                    }
                    else {
                        singleCreateIndex(insertDoc, state->getCollection(), result);
                    }
                    break;
                }
                catch (const StaleConfigException& staleExcep) {
                    result->setError(new WriteErrorDetail);
                    result->getError()->setErrCode(ErrorCodes::StaleShardVersion);
                    buildStaleError(staleExcep.getVersionReceived(),
                                    staleExcep.getVersionWanted(),
                                    result->getError());
                    break;
                }
                catch (const DBException& ex) {
                    Status status(ex.toStatus());
                    if (ErrorCodes::isInterruption(status.code()))
                        throw;
                    result->setError(toWriteError(status));
                    break;
                }
                catch (PageFaultException& pfe) {
                    state->unlock();
                    pfe.touch();
                    continue;  // Try the operation again.
                }
                fassertFailed(17430);
            }
        } // end PageFaultRetryableSection

        // Errors release the write lock, as a matter of policy.
        if (result->getError())
            state->unlock();
    }

    void WriteBatchExecutor::execOneInsert(ExecInsertsState* state, WriteErrorDetail** error) {
        BatchItemRef currInsertItem(state->request, state->currIndex);
        scoped_ptr<CurOp> currentOp(beginCurrentOp(_client, currInsertItem));
        incOpStats(currInsertItem);

        WriteOpResult result;
        insertOne(state, &result);

        if (state->hasLock()) {
            // Normally, unlocking records lock time stats on the active CurOp.  However,
            // insertOne() may not release the lock. In that case, record time by hand.
            state->getLock().recordTime();
            // If we deschedule here, there could be substantial unaccounted locked time.
            // Any time from here will be attributed to the next insert in the batch, or
            // not attributed to any operation if this is the last op in the batch.
            state->getLock().resetTime();
        }

        incWriteStats(currInsertItem,
                      result.getStats(),
                      result.getError(),
                      currentOp.get());
        finishCurrentOp(_client, currentOp.get(), result.getError());

        if (result.getError()) {
            *error = result.releaseError();
        }
    }

    /**
     * Perform a single insert into a collection.  Requires the insert be preprocessed and the
     * collection already has been created.
     *
     * Might fault or error, otherwise populates the result.
     */
    static void singleInsert( const BSONObj& docToInsert,
                              Collection* collection,
                              const PregeneratedKeys* pregen,
                              WriteOpResult* result ) {

        const string& insertNS = collection->ns().ns();

        Lock::assertWriteLocked( insertNS );

        StatusWith<DiskLoc> status = collection->insertDocument( docToInsert, true, pregen );

        if ( !status.isOK() ) {
            result->setError(toWriteError(status.getStatus()));
        }
        else {
            logOp( "i", insertNS.c_str(), docToInsert );
            getDur().commitIfNeeded();
            result->getStats().n = 1;
        }
    }

    /**
     * Perform a single index insert into a collection.  Requires the index descriptor be
     * preprocessed and the collection already has been created.
     *
     * Might fault or error, otherwise populates the result.
     */
    static void singleCreateIndex( const BSONObj& indexDesc,
                                   Collection* collection,
                                   WriteOpResult* result ) {

        const string indexNS = collection->ns().getSystemIndexesCollection();

        Lock::assertWriteLocked( indexNS );

        Status status = collection->getIndexCatalog()->createIndex( indexDesc, true );

        if ( status.code() == ErrorCodes::IndexAlreadyExists ) {
            result->getStats().n = 0;
        }
        else if ( !status.isOK() ) {
            result->setError(toWriteError(status));
        }
        else {
            logOp( "i", indexNS.c_str(), indexDesc );
            result->getStats().n = 1;
        }
    }

    static void multiUpdate( const BatchItemRef& updateItem,
                             WriteOpResult* result ) {

        const NamespaceString nsString(updateItem.getRequest()->getNS());
        UpdateRequest request(nsString);
        request.setQuery(updateItem.getUpdate()->getQuery());
        request.setUpdates(updateItem.getUpdate()->getUpdateExpr());
        request.setMulti(updateItem.getUpdate()->getMulti());
        request.setUpsert(updateItem.getUpdate()->getUpsert());
        request.setUpdateOpLog(true);
        UpdateLifecycleImpl updateLifecycle(true, request.getNamespaceString());
        request.setLifecycle(&updateLifecycle);

        UpdateExecutor executor(&request, &cc().curop()->debug());
        Status status = executor.prepare();
        if (!status.isOK()) {
            result->setError(toWriteError(status));
            return;
        }

        ///////////////////////////////////////////
        Lock::DBWrite writeLock( nsString.ns() );
        ///////////////////////////////////////////

        if ( !checkShardVersion( &shardingState, *updateItem.getRequest(), result ) )
            return;

        Client::Context ctx(nsString.ns(),
                            storageGlobalParams.dbpath);

        try {
            UpdateResult res = executor.execute();

            const long long numDocsModified = res.numDocsModified;
            const long long numMatched = res.numMatched;
            const BSONObj resUpsertedID = res.upserted;

            // We have an _id from an insert
            const bool didInsert = !resUpsertedID.isEmpty();

            result->getStats().nModified = didInsert ? 0 : numDocsModified;
            result->getStats().n = didInsert ? 1 : numMatched;
            result->getStats().upsertedID = resUpsertedID;
        }
        catch (const StaleConfigException& staleExcep) {
            result->setError(new WriteErrorDetail);
            result->getError()->setErrCode(ErrorCodes::StaleShardVersion);
            buildStaleError(staleExcep.getVersionReceived(),
                            staleExcep.getVersionWanted(),
                            result->getError());
        }
        catch (const DBException& ex) {
            status = ex.toStatus();
            if (ErrorCodes::isInterruption(status.code())) {
                throw;
            }
            result->setError(toWriteError(status));
        }
    }

    /**
     * Perform a remove operation, which might remove multiple documents.  Dispatches to remove code
     * currently to do most of this.
     *
     * Might fault or error, otherwise populates the result.
     */
    static void multiRemove( const BatchItemRef& removeItem,
                             WriteOpResult* result ) {

        const NamespaceString nss( removeItem.getRequest()->getNS() );
        DeleteRequest request( nss );
        request.setQuery( removeItem.getDelete()->getQuery() );
        request.setMulti( removeItem.getDelete()->getLimit() != 1 );
        request.setUpdateOpLog(true);
        request.setGod( false );
        DeleteExecutor executor( &request );
        Status status = executor.prepare();
        if ( !status.isOK() ) {
            result->setError(toWriteError(status));
            return;
        }

        ///////////////////////////////////////////
        Lock::DBWrite writeLock( nss.ns() );
        ///////////////////////////////////////////

        // Check version once we're locked

        if ( !checkShardVersion( &shardingState, *removeItem.getRequest(), result ) ) {
            // Version error
            return;
        }

        // Context once we're locked, to set more details in currentOp()
        // TODO: better constructor?
        Client::Context writeContext(nss.ns(),
                                     storageGlobalParams.dbpath);

        try {
            result->getStats().n = executor.execute();
        }
        catch (const StaleConfigException& staleExcep) {
            result->setError(new WriteErrorDetail);
            result->getError()->setErrCode(ErrorCodes::StaleShardVersion);
            buildStaleError(staleExcep.getVersionReceived(),
                            staleExcep.getVersionWanted(),
                            result->getError());
            return;
        }
        catch ( const DBException& ex ) {
            status = ex.toStatus();
            if (ErrorCodes::isInterruption(status.code())) {
                throw;
            }
            result->setError(toWriteError(status));
        }
    }

} // namespace mongo
