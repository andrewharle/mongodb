// dbhelpers.cpp

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

#include "mongo/pch.h"

#include "mongo/db/dbhelpers.h"

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>
#include <fstream>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/db.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/json.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/write_concern.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/s/d_logic.h"

namespace mongo {

    const BSONObj reverseNaturalObj = BSON( "$natural" << -1 );

    void Helpers::ensureIndex(const char *ns, BSONObj keyPattern, bool unique, const char *name) {
        Database* db = cc().database();
        verify(db);

        Collection* collection = db->getCollection( ns );
        if ( !collection )
            return;

        ensureIndex( collection, keyPattern, unique, name );
    }

    void Helpers::ensureIndex(Collection* collection,
                              BSONObj keyPattern, bool unique, const char *name) {
        BSONObjBuilder b;
        b.append("name", name);
        b.append("ns", collection->ns());
        b.append("key", keyPattern);
        b.appendBool("unique", unique);
        BSONObj o = b.done();

        Status status = collection->getIndexCatalog()->createIndex( o, false );
        if ( status.code() == ErrorCodes::IndexAlreadyExists )
            return;
        uassertStatusOK( status );
    }

    /* fetch a single object from collection ns that matches query
       set your db SavedContext first
    */
    bool Helpers::findOne(const StringData& ns, const BSONObj &query, BSONObj& result, bool requireIndex) {
        DiskLoc loc = findOne( ns, query, requireIndex );
        if ( loc.isNull() )
            return false;
        result = loc.obj();
        return true;
    }

    /* fetch a single object from collection ns that matches query
       set your db SavedContext first
    */
    DiskLoc Helpers::findOne(const StringData& ns, const BSONObj &query, bool requireIndex) {
        CanonicalQuery* cq;
        massert(17244, "Could not canonicalize " + query.toString(),
                CanonicalQuery::canonicalize(ns.toString(), query, &cq).isOK());

        Runner* rawRunner;
        size_t options = requireIndex ? QueryPlannerParams::NO_TABLE_SCAN : QueryPlannerParams::DEFAULT;
        massert(17245, "Could not get runner for query " + query.toString(),
                getRunner(cq, &rawRunner, options).isOK());

        auto_ptr<Runner> runner(rawRunner);
        Runner::RunnerState state;
        DiskLoc loc;
        if (Runner::RUNNER_ADVANCED == (state = runner->getNext(NULL, &loc))) {
            return loc;
        }
        return DiskLoc();
    }

    bool Helpers::findById(Client& c, const char *ns, BSONObj query, BSONObj& result ,
                           bool* nsFound , bool* indexFound ) {
        Lock::assertAtLeastReadLocked(ns);
        Database *database = c.database();
        verify( database );

        Collection* collection = database->getCollection( ns );
        if ( !collection ) {
            return false;
        }

        if ( nsFound )
            *nsFound = true;

        IndexCatalog* catalog = collection->getIndexCatalog();
        const IndexDescriptor* desc = catalog->findIdIndex();

        if ( !desc )
            return false;

        if ( indexFound )
            *indexFound = 1;

        // See SERVER-12397.  This may not always be true.
        BtreeBasedAccessMethod* accessMethod =
            static_cast<BtreeBasedAccessMethod*>(catalog->getIndex( desc ));

        DiskLoc loc = accessMethod->findSingle( query["_id"].wrap() );
        if ( loc.isNull() )
            return false;
        result = collection->docFor( loc );
        return true;
    }

    DiskLoc Helpers::findById(Collection* collection, const BSONObj& idquery) {
        verify(collection);
        IndexCatalog* catalog = collection->getIndexCatalog();
        const IndexDescriptor* desc = catalog->findIdIndex();
        uassert(13430, "no _id index", desc);
        // See SERVER-12397.  This may not always be true.
        BtreeBasedAccessMethod* accessMethod =
            static_cast<BtreeBasedAccessMethod*>(catalog->getIndex( desc ));
        return accessMethod->findSingle( idquery["_id"].wrap() );
    }

    vector<BSONObj> Helpers::findAll( const string& ns , const BSONObj& query ) {
        Lock::assertAtLeastReadLocked(ns);
        Client::Context ctx(ns);

        CanonicalQuery* cq;
        uassert(17236, "Could not canonicalize " + query.toString(),
                CanonicalQuery::canonicalize(ns, query, &cq).isOK());

        Runner* rawRunner;
        uassert(17237, "Could not get runner for query " + query.toString(),
                getRunner(cq, &rawRunner).isOK());

        vector<BSONObj> all;

        auto_ptr<Runner> runner(rawRunner);
        Runner::RunnerState state;
        BSONObj obj;
        while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&obj, NULL))) {
            all.push_back(obj);
        }

        return all;
    }

    bool Helpers::isEmpty(const char *ns) {
        Client::Context context(ns, storageGlobalParams.dbpath);
        auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns));
        return Runner::RUNNER_EOF == runner->getNext(NULL, NULL);
    }

    /* Get the first object from a collection.  Generally only useful if the collection
       only ever has a single object -- which is a "singleton collection". Note that the
       BSONObj returned is *not* owned and will become invalid if the database is closed.

       Returns: true if object exists.
    */
    bool Helpers::getSingleton(const char *ns, BSONObj& result) {
        Client::Context context(ns);
        auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns));
        Runner::RunnerState state = runner->getNext(&result, NULL);
        context.getClient()->curop()->done();
        return Runner::RUNNER_ADVANCED == state;
    }

    bool Helpers::getLast(const char *ns, BSONObj& result) {
        Client::Context ctx(ns);
        auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns, InternalPlanner::BACKWARD));
        Runner::RunnerState state = runner->getNext(&result, NULL);
        return Runner::RUNNER_ADVANCED == state;
    }

    void Helpers::upsert( const string& ns , const BSONObj& o, bool fromMigrate ) {
        BSONElement e = o["_id"];
        verify( e.type() );
        BSONObj id = e.wrap();

        OpDebug debug;
        Client::Context context(ns);

        const NamespaceString requestNs(ns);
        UpdateRequest request(requestNs);

        request.setQuery(id);
        request.setUpdates(o);
        request.setUpsert();
        request.setUpdateOpLog();
        request.setFromMigration(fromMigrate);
        UpdateLifecycleImpl updateLifecycle(true, requestNs);
        request.setLifecycle(&updateLifecycle);

        update(request, &debug);
    }

    void Helpers::putSingleton(const char *ns, BSONObj obj) {
        OpDebug debug;
        Client::Context context(ns);

        const NamespaceString requestNs(ns);
        UpdateRequest request(requestNs);

        request.setUpdates(obj);
        request.setUpsert();
        request.setUpdateOpLog();
        UpdateLifecycleImpl updateLifecycle(true, requestNs);
        request.setLifecycle(&updateLifecycle);

        update(request, &debug);

        context.getClient()->curop()->done();
    }

    void Helpers::putSingletonGod(const char *ns, BSONObj obj, bool logTheOp) {
        OpDebug debug;
        Client::Context context(ns);

        const NamespaceString requestNs(ns);
        UpdateRequest request(requestNs);

        request.setGod();
        request.setUpdates(obj);
        request.setUpsert();
        request.setUpdateOpLog(logTheOp);

        update(request, &debug);

        context.getClient()->curop()->done();
    }

    BSONObj Helpers::toKeyFormat( const BSONObj& o ) {
        BSONObjBuilder keyObj( o.objsize() );
        BSONForEach( e , o ) {
            keyObj.appendAs( e , "" );
        }
        return keyObj.obj();
    }

    BSONObj Helpers::inferKeyPattern( const BSONObj& o ) {
        BSONObjBuilder kpBuilder;
        BSONForEach( e , o ) {
            kpBuilder.append( e.fieldName() , 1 );
        }
        return kpBuilder.obj();
    }

    bool findShardKeyIndexPattern_inlock( const string& ns,
                                          const BSONObj& shardKeyPattern,
                                          BSONObj* indexPattern ) {
        verify( Lock::isLocked() );
        Collection* collection = cc().database()->getCollection( ns );
        if ( !collection )
            return false;

        // Allow multiKey based on the invariant that shard keys must be single-valued.
        // Therefore, any multi-key index prefixed by shard key cannot be multikey over
        // the shard key fields.
        const IndexDescriptor* idx =
            collection->getIndexCatalog()->findIndexByPrefix(shardKeyPattern,
                                                             false /* allow multi key */);

        if ( idx == NULL )
            return false;
        *indexPattern = idx->keyPattern().getOwned();
        return true;
    }

    bool findShardKeyIndexPattern( const string& ns,
                                   const BSONObj& shardKeyPattern,
                                   BSONObj* indexPattern ) {
        Client::ReadContext context( ns );
        return findShardKeyIndexPattern_inlock( ns, shardKeyPattern, indexPattern );
    }

    long long Helpers::removeRange( const KeyRange& range,
                                    bool maxInclusive,
                                    bool secondaryThrottle,
                                    RemoveSaver* callback,
                                    bool fromMigrate,
                                    bool onlyRemoveOrphanedDocs )
    {
        Timer rangeRemoveTimer;
        const string& ns = range.ns;

        // The IndexChunk has a keyPattern that may apply to more than one index - we need to
        // select the index and get the full index keyPattern here.
        BSONObj indexKeyPatternDoc;
        if ( !findShardKeyIndexPattern( ns,
                                        range.keyPattern,
                                        &indexKeyPatternDoc ) )
        {
            warning() << "no index found to clean data over range of type "
                      << range.keyPattern << " in " << ns << endl;
            return -1;
        }

        KeyPattern indexKeyPattern( indexKeyPatternDoc );

        // Extend bounds to match the index we found

        // Extend min to get (min, MinKey, MinKey, ....)
        const BSONObj& min =
                Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(range.minKey,
                                                                                   false));
        // If upper bound is included, extend max to get (max, MaxKey, MaxKey, ...)
        // If not included, extend max to get (max, MinKey, MinKey, ....)
        const BSONObj& max =
                Helpers::toKeyFormat( indexKeyPattern.extendRangeBound(range.maxKey,maxInclusive));

        LOG(1) << "begin removal of " << min << " to " << max << " in " << ns
               << (secondaryThrottle ? " (waiting for secondaries)" : "" ) << endl;

        Client& c = cc();

        long long numDeleted = 0;
        
        long long millisWaitingForReplication = 0;

        while ( 1 ) {
            // Scoping for write lock.
            {
                Client::WriteContext ctx(ns);
                Collection* collection = ctx.ctx().db()->getCollection( ns );
                if ( !collection ) break;

                int oldYieldCount = c.curop()->numYields();

                IndexDescriptor* desc =
                    collection->getIndexCatalog()->findIndexByKeyPattern( indexKeyPattern.toBSON() );

                auto_ptr<Runner> runner(InternalPlanner::indexScan(collection, desc, min, max,
                                                                   maxInclusive,
                                                                   InternalPlanner::FORWARD,
                                                                   InternalPlanner::IXSCAN_FETCH));

                runner->setYieldPolicy(Runner::YIELD_AUTO);

                DiskLoc rloc;
                BSONObj obj;
                Runner::RunnerState state;
                // This may yield so we cannot touch nsd after this.
                state = runner->getNext(&obj, &rloc);
                runner.reset();
                if (Runner::RUNNER_EOF == state) { break; }

                if (Runner::RUNNER_DEAD == state) {
                    warning() << "cursor died: aborting deletion for "
                              << min << " to " << max << " in " << ns
                              << endl;
                    break;
                }

                if (Runner::RUNNER_ERROR == state) {
                    warning() << "cursor error while trying to delete "
                              << min << " to " << max
                              << " in " << ns << ": "
                              << WorkingSetCommon::toStatusString(obj) << endl;
                    break;
                }

                verify(Runner::RUNNER_ADVANCED == state);

                int newYieldCount = c.curop()->numYields();
                if (oldYieldCount != newYieldCount && !_isMaster()) {
                    warning() << "current node is not primary anymore, "
                              << "aborting removeRange" << endl;
                    return numDeleted;
                }

                if ( onlyRemoveOrphanedDocs ) {
                    // Do a final check in the write lock to make absolutely sure that our
                    // collection hasn't been modified in a way that invalidates our migration
                    // cleanup.

                    // We should never be able to turn off the sharding state once enabled, but
                    // in the future we might want to.
                    verify(shardingState.enabled());

                    // In write lock, so will be the most up-to-date version
                    CollectionMetadataPtr metadataNow = shardingState.getCollectionMetadata( ns );

                    bool docIsOrphan;
                    if ( metadataNow ) {
                        KeyPattern kp( metadataNow->getKeyPattern() );
                        BSONObj key = kp.extractSingleKey( obj );
                        docIsOrphan = !metadataNow->keyBelongsToMe( key )
                            && !metadataNow->keyIsPending( key );
                    }
                    else {
                        docIsOrphan = false;
                    }

                    if ( !docIsOrphan ) {
                        warning() << "aborting migration cleanup for chunk " << min << " to " << max
                                  << ( metadataNow ? (string) " at document " + obj.toString() : "" )
                                  << ", collection " << ns << " has changed " << endl;
                        break;
                    }
                }

                if ( callback )
                    callback->goingToDelete( obj );

                logOp("d", ns.c_str(), obj["_id"].wrap(), 0, 0, fromMigrate);
                c.database()->getCollection( ns )->deleteDocument( rloc );
                numDeleted++;
            }

            Timer secondaryThrottleTime;

            if ( secondaryThrottle && numDeleted > 0 ) {
                if ( ! waitForReplication( c.getLastOp(), 2, 60 /* seconds to wait */ ) ) {
                    warning() << "replication to secondaries for removeRange at least 60 seconds behind" << endl;
                }
                millisWaitingForReplication += secondaryThrottleTime.millis();
            }
            
            if ( ! Lock::isLocked() ) {
                int micros = ( 2 * Client::recommendedYieldMicros() ) - secondaryThrottleTime.micros();
                if ( micros > 0 ) {
                    LOG(1) << "Helpers::removeRangeUnlocked going to sleep for " << micros << " micros" << endl;
                    sleepmicros( micros );
                }
            }
        }
        
        if ( secondaryThrottle )
            log() << "Helpers::removeRangeUnlocked time spent waiting for replication: "  
                  << millisWaitingForReplication << "ms" << endl;
        
        LOG(1) << "end removal of " << min << " to " << max << " in " << ns
               << " (took " << rangeRemoveTimer.millis() << "ms)" << endl;

        return numDeleted;
    }

    const long long Helpers::kMaxDocsPerChunk( 250000 );

    // Used by migration clone step
    // TODO: Cannot hook up quite yet due to _trackerLocks in shared migration code.
    Status Helpers::getLocsInRange( const KeyRange& range,
                                    long long maxChunkSizeBytes,
                                    set<DiskLoc>* locs,
                                    long long* numDocs,
                                    long long* estChunkSizeBytes )
    {
        const string ns = range.ns;
        *estChunkSizeBytes = 0;
        *numDocs = 0;

        Client::ReadContext ctx( ns );
        Collection* collection = ctx.ctx().db()->getCollection( ns );
        if ( !collection ) return Status( ErrorCodes::NamespaceNotFound, ns );

        // Require single key

        IndexDescriptor *idx =
            collection->getIndexCatalog()->findIndexByPrefix( range.keyPattern, true );

        if ( idx == NULL ) {
            return Status( ErrorCodes::IndexNotFound, range.keyPattern.toString() );
        }

        // use the average object size to estimate how many objects a full chunk would carry
        // do that while traversing the chunk's range using the sharding index, below
        // there's a fair amount of slack before we determine a chunk is too large because object
        // sizes will vary
        long long avgDocsWhenFull;
        long long avgDocSizeBytes;
        const long long totalDocsInNS = collection->numRecords();
        if ( totalDocsInNS > 0 ) {
            // TODO: Figure out what's up here
            avgDocSizeBytes = collection->details()->dataSize() / totalDocsInNS;
            avgDocsWhenFull = maxChunkSizeBytes / avgDocSizeBytes;
            avgDocsWhenFull = std::min( kMaxDocsPerChunk + 1,
                                        130 * avgDocsWhenFull / 100 /* slack */);
        }
        else {
            avgDocSizeBytes = 0;
            avgDocsWhenFull = kMaxDocsPerChunk + 1;
        }

        // Assume both min and max non-empty, append MinKey's to make them fit chosen index
        KeyPattern idxKeyPattern( idx->keyPattern() );
        BSONObj min = Helpers::toKeyFormat( idxKeyPattern.extendRangeBound( range.minKey, false ) );
        BSONObj max = Helpers::toKeyFormat( idxKeyPattern.extendRangeBound( range.maxKey, false ) );


        // do a full traversal of the chunk and don't stop even if we think it is a large chunk
        // we want the number of records to better report, in that case
        bool isLargeChunk = false;
        long long docCount = 0;

        auto_ptr<Runner> runner(InternalPlanner::indexScan(collection, idx, min, max, false));
        // we can afford to yield here because any change to the base data that we might miss  is
        // already being queued and will be migrated in the 'transferMods' stage
        runner->setYieldPolicy(Runner::YIELD_AUTO);

        DiskLoc loc;
        Runner::RunnerState state;
        while (Runner::RUNNER_ADVANCED == (state = runner->getNext(NULL, &loc))) {
            if ( !isLargeChunk ) {
                locs->insert( loc );
            }

            if ( ++docCount > avgDocsWhenFull ) {
                isLargeChunk = true;
            }
        }

        *numDocs = docCount;
        *estChunkSizeBytes = docCount * avgDocSizeBytes;

        if ( isLargeChunk ) {
            stringstream ss;
            ss << estChunkSizeBytes;
            return Status( ErrorCodes::InvalidLength, ss.str() );
        }

        return Status::OK();
    }


    void Helpers::emptyCollection(const char *ns) {
        Client::Context context(ns);
        deleteObjects(ns, BSONObj(), false);
    }

    Helpers::RemoveSaver::RemoveSaver( const string& a , const string& b , const string& why) 
        : _out(0) {
        static int NUM = 0;

        _root = storageGlobalParams.dbpath;
        if ( a.size() )
            _root /= a;
        if ( b.size() )
            _root /= b;
        verify( a.size() || b.size() );

        _file = _root;

        stringstream ss;
        ss << why << "." << terseCurrentTime(false) << "." << NUM++ << ".bson";
        _file /= ss.str();
    }

    Helpers::RemoveSaver::~RemoveSaver() {
        if ( _out ) {
            _out->close();
            delete _out;
            _out = 0;
        }
    }

    void Helpers::RemoveSaver::goingToDelete( const BSONObj& o ) {
        if ( ! _out ) {
            boost::filesystem::create_directories( _root );
            _out = new ofstream();
            _out->open( _file.string().c_str() , ios_base::out | ios_base::binary );
            if ( ! _out->good() ) {
                error() << "couldn't create file: " << _file.string() << 
                    " for remove saving" << endl;
                delete _out;
                _out = 0;
                return;
            }

        }
        _out->write( o.objdata() , o.objsize() );
    }


} // namespace mongo
