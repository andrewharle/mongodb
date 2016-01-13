// index_create.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/catalog/index_create.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/structure/btree/btreebuilder.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/curop.h"
#include "mongo/db/extsort.h"
#include "mongo/db/structure/catalog/index_details.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/runner_yield_policy.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

    /**
     * Add the provided (obj, dl) pair to the provided index.
     */
    static void addKeysToIndex( Collection* collection,
                                const IndexDescriptor* descriptor,
                                IndexAccessMethod* accessMethod,
                                const BSONObj& obj, const DiskLoc &recordLoc ) {

        InsertDeleteOptions options;
        options.logIfError = false;
        options.dupsAllowed = true;

        if ( descriptor->isIdIndex() || descriptor->unique() ) {
            if ( !ignoreUniqueIndex( descriptor ) ) {
                options.dupsAllowed = false;
            }
        }

        int64_t inserted;
        Status ret = accessMethod->insert(obj, recordLoc, options, &inserted);
        uassertStatusOK( ret );
    }

    unsigned long long addExistingToIndex( Collection* collection,
                                           const IndexDescriptor* descriptor,
                                           IndexAccessMethod* accessMethod,
                                           bool shouldYield ) {

        string ns = collection->ns().ns(); // our copy for sanity

        bool dupsAllowed = !descriptor->unique();
        bool dropDups = descriptor->dropDups();


        string curopMessage;
        {
            stringstream ss;
            ss << "Index Build";
            if ( shouldYield )
                ss << "(background)";
            curopMessage = ss.str();
        }

        ProgressMeter& progress =
            cc().curop()->setMessage( curopMessage.c_str(),
                                      curopMessage,
                                      collection->numRecords() );

        unsigned long long n = 0;
        unsigned long long numDropped = 0;

        auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns));

        // We're not delegating yielding to the runner because we need to know when a yield
        // happens.
        RunnerYieldPolicy yieldPolicy;

        std::string idxName = descriptor->indexName();

        // After this yields in the loop, idx may point at a different index (if indexes get
        // flipped, see insert_makeIndex) or even an empty IndexDetails, so nothing below should
        // depend on idx. idxNo should be recalculated after each yield.

        BSONObj js;
        DiskLoc loc;
        while (Runner::RUNNER_ADVANCED == runner->getNext(&js, &loc)) {
            try {
                if ( !dupsAllowed && dropDups ) {
                    LastError::Disabled led( lastError.get() );
                    addKeysToIndex(collection, descriptor, accessMethod, js, loc);
                }
                else {
                    addKeysToIndex(collection, descriptor, accessMethod, js, loc);
                }
            }
            catch( AssertionException& e ) {
                if (ErrorCodes::isInterruption(DBException::convertExceptionCode(e.getCode()))) {
                    killCurrentOp.checkForInterrupt();
                }

                // TODO: Does exception really imply dropDups exception?
                if (dropDups) {
                    bool runnerEOF = runner->isEOF();
                    runner->saveState();
                    BSONObj toDelete;
                    collection->deleteDocument( loc, false, true, &toDelete );
                    logOp( "d", ns.c_str(), toDelete );

                    if (!runner->restoreState()) {
                        // Runner got killed somehow.  This probably shouldn't happen.
                        if (runnerEOF) {
                            // Quote: "We were already at the end.  Normal.
                            // TODO: Why is this normal?
                        }
                        else {
                            uasserted(ErrorCodes::CursorNotFound, 
                                      "cursor gone during bg index; dropDups");
                        }
                        break;
                    }
                    // We deleted a record, but we didn't actually yield the dblock.
                    // TODO: Why did the old code assume we yielded the lock?
                    numDropped++;
                }
                else {
                    log() << "background addExistingToIndex exception " << e.what() << endl;
                    throw;
                }
            }

            n++;
            progress.hit();

            getDur().commitIfNeeded();
            if (shouldYield && yieldPolicy.shouldYield()) {
                // Note: yieldAndCheckIfOK checks for interrupt and thus can throw
                if (!yieldPolicy.yieldAndCheckIfOK(runner.get())) {
                    uasserted(ErrorCodes::CursorNotFound, "cursor gone during bg index");
                    break;
                }

                // Checking for interrupt here is necessary because the bg index 
                // interruptors can only interrupt this index build while they hold 
                // a write lock, and yieldAndCheckIfOK only checks for
                // interrupt prior to yielding our write lock. We need to check the kill flag
                // here before another iteration of the loop.
                killCurrentOp.checkForInterrupt();

                progress.setTotalWhileRunning( collection->numRecords() );
                // Recalculate idxNo if we yielded
                IndexDescriptor* idx = collection->getIndexCatalog()->findIndexByName( idxName,
                                                                                       true );
                verify( idx && idx == descriptor );
            }
        }

        progress.finished();
        if ( dropDups && numDropped )
            log() << "\t index build dropped: " << numDropped << " dups";
        return n;
    }

    // ---------------------------

    // throws DBException
    void buildAnIndex( Collection* collection,
                       IndexCatalogEntry* btreeState,
                       bool mayInterrupt ) {

        string ns = collection->ns().ns(); // our copy
        const IndexDescriptor* idx = btreeState->descriptor();
        const BSONObj& idxInfo = idx->infoObj();

        MONGO_TLOG(0) << "build index on: " << ns
                      << " properties: " << idx->toString() << endl;
        audit::logCreateIndex( currentClient.get(), &idxInfo, idx->indexName(), ns );

        Timer t;

        verify( Lock::isWriteLocked( ns ) );
        // this is so that people know there are more keys to look at when doing
        // things like in place updates, etc...
        collection->infoCache()->addedIndex();

        if ( collection->numRecords() == 0 ) {
            Status status = btreeState->accessMethod()->initializeAsEmpty();
            massert( 17343,
                     str::stream() << "IndexAccessMethod::initializeAsEmpty failed" << status.toString(),
                     status.isOK() );
            MONGO_TLOG(0) << "\t added index to empty collection";
            return;
        }

        scoped_ptr<BackgroundOperation> backgroundOperation;
        bool doInBackground = false;

        if ( idxInfo["background"].trueValue() && !inDBRepair && mayInterrupt ) {
            doInBackground = true;
            backgroundOperation.reset( new BackgroundOperation(ns) );
            uassert( 13130,
                     "can't start bg index b/c in recursive lock (db.eval?)",
                     !Lock::nested() );
            log() << "\t building index in background";
        }

        Status status = btreeState->accessMethod()->initializeAsEmpty();
        massert( 17342,
                 str::stream()
                 << "IndexAccessMethod::initializeAsEmpty failed"
                 << status.toString(),
                 status.isOK() );

        IndexAccessMethod* bulk = doInBackground ? NULL : btreeState->accessMethod()->initiateBulk();
        scoped_ptr<IndexAccessMethod> bulkHolder(bulk);
        IndexAccessMethod* iam = bulk ? bulk : btreeState->accessMethod();

        if ( bulk )
            log() << "\t building index using bulk method";

        unsigned long long n = addExistingToIndex( collection,
                                                   btreeState->descriptor(),
                                                   iam,
                                                   doInBackground );

        if ( bulk ) {
            LOG(1) << "\t bulk commit starting";
            std::set<DiskLoc> dupsToDrop;

            Status status = btreeState->accessMethod()->commitBulk( bulk,
                                                                    mayInterrupt,
                                                                    &dupsToDrop );
            massert( 17398,
                     str::stream() << "commitBulk failed: " << status.toString(),
                     status.isOK() );

            if ( dupsToDrop.size() )
                log() << "\t bulk dropping " << dupsToDrop.size() << " dups";

            for( set<DiskLoc>::const_iterator i = dupsToDrop.begin(); i != dupsToDrop.end(); ++i ) {
                BSONObj toDelete;
                collection->deleteDocument( *i,
                                            false /* cappedOk */,
                                            true /* noWarn */,
                                            &toDelete );
                if ( isMaster( ns.c_str() ) ) {
                    logOp( "d", ns.c_str(), toDelete );
                }
                
                getDur().commitIfNeeded();

                RARELY if ( mayInterrupt ) {
                    killCurrentOp.checkForInterrupt();
                }
            }
        }

        verify( !btreeState->head().isNull() );
        MONGO_TLOG(0) << "build index done.  scanned " << n << " total records. "
                      << t.millis() / 1000.0 << " secs" << endl;

        // this one is so people know that the index is finished
        collection->infoCache()->addedIndex();
    }

    // ----------------------------

    MultiIndexBlock::MultiIndexBlock( Collection* collection )
        : _collection( collection ) {
    }

    MultiIndexBlock::~MultiIndexBlock() {
        for ( size_t i = 0; i < _states.size(); i++ ) {
            delete _states[i].bulk;
            delete _states[i].block;
        }
    }

    Status MultiIndexBlock::init(std::vector<BSONObj>& indexSpecs) {

        for ( size_t i = 0; i < indexSpecs.size(); i++ ) {
            BSONObj info = indexSpecs[i];

            string pluginName = IndexNames::findPluginName( info["key"].Obj() );
            if ( pluginName.size() ) {
                Status s =
                    _collection->getIndexCatalog()->_upgradeDatabaseMinorVersionIfNeeded(pluginName);
                if ( !s.isOK() )
                    return s;
            }

        }

        for ( size_t i = 0; i < indexSpecs.size(); i++ ) {
            BSONObj info = indexSpecs[i];
            StatusWith<BSONObj> statusWithInfo =
                _collection->getIndexCatalog()->prepareSpecForCreate( info );
            Status status = statusWithInfo.getStatus();
            if ( !status.isOK() )
                return status;
            info = statusWithInfo.getValue();

            IndexState state;
            state.block = new IndexCatalog::IndexBuildBlock( _collection, info );
            status = state.block->init();
            if ( !status.isOK() )
                return status;

            state.real = state.block->getEntry()->accessMethod();
            status = state.real->initializeAsEmpty();
            if ( !status.isOK() )
                return status;

            state.bulk = state.real->initiateBulk();

            _states.push_back( state );
        }

        return Status::OK();
    }

    Status MultiIndexBlock::insert( const BSONObj& doc,
                                    const DiskLoc& loc,
                                    const InsertDeleteOptions& options ) {

        for ( size_t i = 0; i < _states.size(); i++ ) {
            Status idxStatus = _states[i].forInsert()->insert( doc,
                                                               loc,
                                                               options,
                                                               NULL );
            if ( !idxStatus.isOK() )
                return idxStatus;
        }
        return Status::OK();
    }

    Status MultiIndexBlock::commit() {
        for ( size_t i = 0; i < _states.size(); i++ ) {
            if ( _states[i].bulk == NULL )
                continue;
            Status status = _states[i].real->commitBulk( _states[i].bulk,
                                                         false,
                                                         NULL );
            if ( !status.isOK() )
                return status;
        }

        for ( size_t i = 0; i < _states.size(); i++ ) {
            _states[i].block->success();
        }

        return Status::OK();
    }

}  // namespace mongo

