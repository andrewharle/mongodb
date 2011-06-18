// d_migrate.cpp

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
*/


/**
   these are commands that live in mongod
   mostly around shard management and checking
 */

#include "pch.h"
#include <map>
#include <string>
#include <algorithm>

#include "../db/commands.h"
#include "../db/jsobj.h"
#include "../db/dbmessage.h"
#include "../db/query.h"
#include "../db/cmdline.h"
#include "../db/queryoptimizer.h"
#include "../db/btree.h"
#include "../db/repl_block.h"
#include "../db/dur.h"

#include "../client/connpool.h"
#include "../client/distlock.h"

#include "../util/queue.h"
#include "../util/unittest.h"
#include "../util/processinfo.h"

#include "shard.h"
#include "d_logic.h"
#include "config.h"
#include "chunk.h"

using namespace std;

namespace mongo {

    class MoveTimingHelper {
    public:
        MoveTimingHelper( const string& where , const string& ns , BSONObj min , BSONObj max , int total )
            : _where( where ) , _ns( ns ) , _next( 0 ) , _total( total ) {
            _nextNote = 0;
            _b.append( "min" , min );
            _b.append( "max" , max );
        }

        ~MoveTimingHelper() {
            // even if logChange doesn't throw, bson does
            // sigh
            try {
                if ( _next != _total ) {
                    note( "aborted" );
                }
                configServer.logChange( (string)"moveChunk." + _where , _ns, _b.obj() );
            }
            catch ( const std::exception& e ) {
                log( LL_WARNING ) << "couldn't record timing for moveChunk '" << _where << "': " << e.what() << endl;
            }
        }

        void done( int step ) {
            assert( step == ++_next );
            assert( step <= _total );

            stringstream ss;
            ss << "step" << step;
            string s = ss.str();

            CurOp * op = cc().curop();
            if ( op )
                op->setMessage( s.c_str() );
            else
                log( LL_WARNING ) << "op is null in MoveTimingHelper::done" << endl;

            _b.appendNumber( s , _t.millis() );
            _t.reset();

#if 0
            // debugging for memory leak?
            ProcessInfo pi;
            ss << " v:" << pi.getVirtualMemorySize()
               << " r:" << pi.getResidentSize();
            log() << ss.str() << endl;
#endif
        }


        void note( const string& s ) {
            string field = "note";
            if ( _nextNote > 0 ) {
                StringBuilder buf;
                buf << "note" << _nextNote;
                field = buf.str();
            }
            _nextNote++;

            _b.append( field , s );
        }

    private:
        Timer _t;

        string _where;
        string _ns;

        int _next;
        int _total; // expected # of steps
        int _nextNote;

        BSONObjBuilder _b;

    };

    struct OldDataCleanup {
        static AtomicUInt _numThreads; // how many threads are doing async cleanusp

        string ns;
        BSONObj min;
        BSONObj max;
        set<CursorId> initial;

        OldDataCleanup(){
            _numThreads++;
        }
        OldDataCleanup( const OldDataCleanup& other ) {
            ns = other.ns;
            min = other.min.getOwned();
            max = other.max.getOwned();
            initial = other.initial;
            _numThreads++;
        }
        ~OldDataCleanup(){
            _numThreads--;
        }

        void doRemove() {
            ShardForceVersionOkModeBlock sf;
            writelock lk(ns);
            RemoveSaver rs("moveChunk",ns,"post-cleanup");
            long long num = Helpers::removeRange( ns , min , max , true , false , cmdLine.moveParanoia ? &rs : 0 );
            log() << "moveChunk deleted: " << num << endl;
        }

    };

    AtomicUInt OldDataCleanup::_numThreads = 0;

    static const char * const cleanUpThreadName = "cleanupOldData";

    void _cleanupOldData( OldDataCleanup cleanup ) {
        Client::initThread( cleanUpThreadName );
        log() << " (start) waiting to cleanup " << cleanup.ns << " from " << cleanup.min << " -> " << cleanup.max << "  # cursors:" << cleanup.initial.size() << endl;

        int loops = 0;
        Timer t;
        while ( t.seconds() < 900 ) { // 15 minutes
            assert( dbMutex.getState() == 0 );
            sleepmillis( 20 );

            set<CursorId> now;
            ClientCursor::find( cleanup.ns , now );

            set<CursorId> left;
            for ( set<CursorId>::iterator i=cleanup.initial.begin(); i!=cleanup.initial.end(); ++i ) {
                CursorId id = *i;
                if ( now.count(id) )
                    left.insert( id );
            }

            if ( left.size() == 0 )
                break;
            cleanup.initial = left;

            if ( ( loops++ % 200 ) == 0 ) {
                log() << " (looping " << loops << ") waiting to cleanup " << cleanup.ns << " from " << cleanup.min << " -> " << cleanup.max << "  # cursors:" << cleanup.initial.size() << endl;

                stringstream ss;
                for ( set<CursorId>::iterator i=cleanup.initial.begin(); i!=cleanup.initial.end(); ++i ) {
                    CursorId id = *i;
                    ss << id << " ";
                }
                log() << " cursors: " << ss.str() << endl;
            }
        }

        cleanup.doRemove();

        cc().shutdown();
    }

    void cleanupOldData( OldDataCleanup cleanup ) {
        try {
            _cleanupOldData( cleanup );
        }
        catch ( std::exception& e ) {
            log() << " error cleaning old data:" << e.what() << endl;
        }
        catch ( ... ) {
            log() << " unknown error cleaning old data" << endl;
        }
    }

    class ChunkCommandHelper : public Command {
    public:
        ChunkCommandHelper( const char * name )
            : Command( name ) {
        }

        virtual void help( stringstream& help ) const {
            help << "internal - should not be called directly" << endl;
        }
        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; }

    };

    bool isInRange( const BSONObj& obj , const BSONObj& min , const BSONObj& max ) {
        BSONObj k = obj.extractFields( min, true );

        return k.woCompare( min ) >= 0 && k.woCompare( max ) < 0;
    }


    class MigrateFromStatus {
    public:

        MigrateFromStatus() : _m("MigrateFromStatus") {
            _active = false;
            _inCriticalSection = false;
            _memoryUsed = 0;
        }

        void start( string ns , const BSONObj& min , const BSONObj& max ) {
            scoped_lock l(_m); // reads and writes _active

            assert( ! _active );

            assert( ! min.isEmpty() );
            assert( ! max.isEmpty() );
            assert( ns.size() );

            _ns = ns;
            _min = min;
            _max = max;

            assert( _cloneLocs.size() == 0 );
            assert( _deleted.size() == 0 );
            assert( _reload.size() == 0 );
            assert( _memoryUsed == 0 );

            _active = true;
        }

        void done() {
            readlock lk( _ns );

            {
                scoped_spinlock lk( _trackerLocks );
                _deleted.clear();
                _reload.clear();
                _cloneLocs.clear();
            }
            _memoryUsed = 0;

            scoped_lock l(_m);
            _active = false;
            _inCriticalSection = false;
        }

        void logOp( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt ) {
            if ( ! _getActive() )
                return;

            if ( _ns != ns )
                return;

            // no need to log if this is not an insertion, an update, or an actual deletion
            // note: opstr 'db' isn't a deletion but a mention that a database exists (for replication
            // machinery mostly)
            char op = opstr[0];
            if ( op == 'n' || op =='c' || ( op == 'd' && opstr[1] == 'b' ) )
                return;

            BSONElement ide;
            if ( patt )
                ide = patt->getField( "_id" );
            else
                ide = obj["_id"];

            if ( ide.eoo() ) {
                log( LL_WARNING ) << "logOpForSharding got mod with no _id, ignoring  obj: " << obj << endl;
                return;
            }

            BSONObj it;

            switch ( opstr[0] ) {

            case 'd': {

                if ( getThreadName() == cleanUpThreadName ) {
                    // we don't want to xfer things we're cleaning
                    // as then they'll be deleted on TO
                    // which is bad
                    return;
                }

                // can't filter deletes :(
                _deleted.push_back( ide.wrap() );
                _memoryUsed += ide.size() + 5;
                return;
            }

            case 'i':
                it = obj;
                break;

            case 'u':
                if ( ! Helpers::findById( cc() , _ns.c_str() , ide.wrap() , it ) ) {
                    log( LL_WARNING ) << "logOpForSharding couldn't find: " << ide << " even though should have" << endl;
                    return;
                }
                break;

            }

            if ( ! isInRange( it , _min , _max ) )
                return;

            _reload.push_back( ide.wrap() );
            _memoryUsed += ide.size() + 5;
        }

        void xfer( list<BSONObj> * l , BSONObjBuilder& b , const char * name , long long& size , bool explode ) {
            const long long maxSize = 1024 * 1024;

            if ( l->size() == 0 || size > maxSize )
                return;

            BSONArrayBuilder arr(b.subarrayStart(name));

            list<BSONObj>::iterator i = l->begin();

            while ( i != l->end() && size < maxSize ) {
                BSONObj t = *i;
                if ( explode ) {
                    BSONObj it;
                    if ( Helpers::findById( cc() , _ns.c_str() , t, it ) ) {
                        arr.append( it );
                        size += it.objsize();
                    }
                }
                else {
                    arr.append( t );
                }
                i = l->erase( i );
                size += t.objsize();
            }

            arr.done();
        }

        /**
         * called from the dest of a migrate
         * transfers mods from src to dest
         */
        bool transferMods( string& errmsg , BSONObjBuilder& b ) {
            if ( ! _getActive() ) {
                errmsg = "no active migration!";
                return false;
            }

            long long size = 0;

            {
                readlock rl( _ns );
                Client::Context cx( _ns );

                xfer( &_deleted , b , "deleted" , size , false );
                xfer( &_reload , b , "reload" , size , true );
            }

            b.append( "size" , size );

            return true;
        }

        /**
         * Get the disklocs that belong to the chunk migrated and sort them in _cloneLocs (to avoid seeking disk later)
         *
         * @param maxChunkSize number of bytes beyond which a chunk's base data (no indices) is considered too large to move
         * @param errmsg filled with textual description of error if this call return false
         * @return false if approximate chunk size is too big to move or true otherwise
         */
        bool storeCurrentLocs( long long maxChunkSize , string& errmsg , BSONObjBuilder& result ) {
            readlock l( _ns );
            Client::Context ctx( _ns );
            NamespaceDetails *d = nsdetails( _ns.c_str() );
            if ( ! d ) {
                errmsg = "ns not found, should be impossible";
                return false;
            }

            BSONObj keyPattern;
            // the copies are needed because the indexDetailsForRange destroys the input
            BSONObj min = _min.copy();
            BSONObj max = _max.copy();
            IndexDetails *idx = indexDetailsForRange( _ns.c_str() , errmsg , min , max , keyPattern );
            if ( idx == NULL ) {
                errmsg = "can't find index in storeCurrentLocs";
                return false;
            }

            scoped_ptr<ClientCursor> cc( new ClientCursor( QueryOption_NoCursorTimeout ,
                                         shared_ptr<Cursor>( new BtreeCursor( d , d->idxNo(*idx) , *idx , min , max , false , 1 ) ) ,
                                         _ns ) );

            // use the average object size to estimate how many objects a full chunk would carry
            // do that while traversing the chunk's range using the sharding index, below
            // there's a fair amout of slack before we determine a chunk is too large because object sizes will vary
            unsigned long long maxRecsWhenFull;
            long long avgRecSize;
            const long long totalRecs = d->stats.nrecords;
            if ( totalRecs > 0 ) {
                avgRecSize = d->stats.datasize / totalRecs;
                maxRecsWhenFull = maxChunkSize / avgRecSize;
                maxRecsWhenFull = 130 * maxRecsWhenFull / 100; // slack
            }
            else {
                avgRecSize = 0;
                maxRecsWhenFull = numeric_limits<long long>::max();
            }

            // do a full traversal of the chunk and don't stop even if we think it is a large chunk
            // we want the number of records to better report, in that case
            bool isLargeChunk = false;
            unsigned long long recCount = 0;;
            while ( cc->ok() ) {
                DiskLoc dl = cc->currLoc();
                if ( ! isLargeChunk ) {
                    scoped_spinlock lk( _trackerLocks );
                    _cloneLocs.insert( dl );
                }
                cc->advance();

                // we can afford to yield here because any change to the base data that we might miss is already being
                // queued and will be migrated in the 'transferMods' stage
                if ( ! cc->yieldSometimes() ) {
                    break;
                }

                if ( ++recCount > maxRecsWhenFull ) {
                    isLargeChunk = true;
                }
            }

            if ( isLargeChunk ) {
                warning() << "can't move chunk of size (aprox) " << recCount * avgRecSize
                          << " because maximum size allowed to move is " << maxChunkSize
                          << " ns: " << _ns << " " << _min << " -> " << _max
                          << endl;
                result.appendBool( "chunkTooBig" , true );
                result.appendNumber( "chunkSize" , (long long)(recCount * avgRecSize) );
                errmsg = "chunk too big to move";
                return false;
            }

            {
                scoped_spinlock lk( _trackerLocks );
                log() << "moveChunk number of documents: " << _cloneLocs.size() << endl;
            }
            return true;
        }

        bool clone( string& errmsg , BSONObjBuilder& result ) {
            if ( ! _getActive() ) {
                errmsg = "not active";
                return false;
            }

            ElapsedTracker tracker (128, 10); // same as ClientCursor::_yieldSometimesTracker

            int allocSize;
            {
                readlock l(_ns);
                Client::Context ctx( _ns );
                NamespaceDetails *d = nsdetails( _ns.c_str() );
                assert( d );
                scoped_spinlock lk( _trackerLocks );
                allocSize = std::min(BSONObjMaxUserSize, (int)((12 + d->averageObjectSize()) * _cloneLocs.size()));
            }
            BSONArrayBuilder a (allocSize);
            
            while ( 1 ) {
                bool filledBuffer = false;
                
                readlock l( _ns );
                Client::Context ctx( _ns );
                scoped_spinlock lk( _trackerLocks );
                set<DiskLoc>::iterator i = _cloneLocs.begin();
                for ( ; i!=_cloneLocs.end(); ++i ) {
                    if (tracker.ping()) // should I yield?
                        break;

                    DiskLoc dl = *i;
                    BSONObj o = dl.obj();

                    // use the builder size instead of accumulating 'o's size so that we take into consideration
                    // the overhead of BSONArray indices
                    if ( a.len() + o.objsize() + 1024 > BSONObjMaxUserSize ) {
                        filledBuffer = true; // break out of outer while loop
                        break;
                    }

                    a.append( o );
                }

                _cloneLocs.erase( _cloneLocs.begin() , i );

                if ( _cloneLocs.empty() || filledBuffer )
                    break;
            }

            result.appendArray( "objects" , a.arr() );
            return true;
        }

        void aboutToDelete( const Database* db , const DiskLoc& dl ) {
            dbMutex.assertWriteLocked();

            if ( ! _getActive() )
                return;

            if ( ! db->ownsNS( _ns ) )
                return;

            
            // not needed right now
            // but trying to prevent a future bug
            scoped_spinlock lk( _trackerLocks ); 

            _cloneLocs.erase( dl );
        }

        long long mbUsed() const { return _memoryUsed / ( 1024 * 1024 ); }

        bool getInCriticalSection() const { scoped_lock l(_m); return _inCriticalSection; }
        void setInCriticalSection( bool b ) { scoped_lock l(_m); _inCriticalSection = b; }

        bool isActive() const { return _getActive(); }

    private:
        mutable mongo::mutex _m; // protect _inCriticalSection and _active
        bool _inCriticalSection;
        bool _active;

        string _ns;
        BSONObj _min;
        BSONObj _max;

        // we need the lock in case there is a malicious _migrateClone for example
        // even though it shouldn't be needed under normal operation
        SpinLock _trackerLocks;

        // disk locs yet to be transferred from here to the other side
        // no locking needed because built initially by 1 thread in a read lock
        // emptied by 1 thread in a read lock
        // updates applied by 1 thread in a write lock
        set<DiskLoc> _cloneLocs;

        list<BSONObj> _reload; // objects that were modified that must be recloned
        list<BSONObj> _deleted; // objects deleted during clone that should be deleted later
        long long _memoryUsed; // bytes in _reload + _deleted

        bool _getActive() const { scoped_lock l(_m); return _active; }
        void _setActive( bool b ) { scoped_lock l(_m); _active = b; }

    } migrateFromStatus;

    struct MigrateStatusHolder {
        MigrateStatusHolder( string ns , const BSONObj& min , const BSONObj& max ) {
            migrateFromStatus.start( ns , min , max );
        }
        ~MigrateStatusHolder() {
            migrateFromStatus.done();
        }
    };

    void logOpForSharding( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt ) {
        migrateFromStatus.logOp( opstr , ns , obj , patt );
    }

    void aboutToDeleteForSharding( const Database* db , const DiskLoc& dl ) {
        migrateFromStatus.aboutToDelete( db , dl );
    }

    class TransferModsCommand : public ChunkCommandHelper {
    public:
        TransferModsCommand() : ChunkCommandHelper( "_transferMods" ) {}

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            return migrateFromStatus.transferMods( errmsg, result );
        }
    } transferModsCommand;


    class InitialCloneCommand : public ChunkCommandHelper {
    public:
        InitialCloneCommand() : ChunkCommandHelper( "_migrateClone" ) {}

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            return migrateFromStatus.clone( errmsg, result );
        }
    } initialCloneCommand;


    /**
     * this is the main entry for moveChunk
     * called to initial a move
     * usually by a mongos
     * this is called on the "from" side
     */
    class MoveChunkCommand : public Command {
    public:
        MoveChunkCommand() : Command( "moveChunk" ) {}
        virtual void help( stringstream& help ) const {
            help << "should not be calling this directly" << endl;
        }

        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; }


        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            // 1. parse options
            // 2. make sure my view is complete and lock
            // 3. start migrate
            //    in a read lock, get all DiskLoc and sort so we can do as little seeking as possible
            //    tell to start transferring
            // 4. pause till migrate caught up
            // 5. LOCK
            //    a) update my config, essentially locking
            //    b) finish migrate
            //    c) update config server
            //    d) logChange to config server
            // 6. wait for all current cursors to expire
            // 7. remove data locally

            // -------------------------------

            // 1.
            string ns = cmdObj.firstElement().str();
            string to = cmdObj["to"].str();
            string from = cmdObj["from"].str(); // my public address, a tad redundant, but safe
            BSONObj min  = cmdObj["min"].Obj();
            BSONObj max  = cmdObj["max"].Obj();
            BSONElement shardId = cmdObj["shardId"];
            BSONElement maxSizeElem = cmdObj["maxChunkSizeBytes"];

            if ( ns.empty() ) {
                errmsg = "need to specify namespace in command";
                return false;
            }

            if ( to.empty() ) {
                errmsg = "need to specify server to move chunk to";
                return false;
            }
            if ( from.empty() ) {
                errmsg = "need to specify server to move chunk from";
                return false;
            }

            if ( min.isEmpty() ) {
                errmsg = "need to specify a min";
                return false;
            }

            if ( max.isEmpty() ) {
                errmsg = "need to specify a max";
                return false;
            }

            if ( shardId.eoo() ) {
                errmsg = "need shardId";
                return false;
            }

            if ( maxSizeElem.eoo() || ! maxSizeElem.isNumber() ) {
                errmsg = "need to specify maxChunkSizeBytes";
                return false;
            }
            const long long maxChunkSize = maxSizeElem.numberLong(); // in bytes

            if ( ! shardingState.enabled() ) {
                if ( cmdObj["configdb"].type() != String ) {
                    errmsg = "sharding not enabled";
                    return false;
                }
                string configdb = cmdObj["configdb"].String();
                shardingState.enable( configdb );
                configServer.init( configdb );
            }

            MoveTimingHelper timing( "from" , ns , min , max , 6 /* steps */);

            Shard fromShard( from );
            Shard toShard( to );

            log() << "received moveChunk request: " << cmdObj << endl;

            timing.done(1);

            // 2.
            DistributedLock lockSetup( ConnectionString( shardingState.getConfigServer() , ConnectionString::SYNC ) , ns );
            dist_lock_try dlk( &lockSetup , (string)"migrate-" + min.toString() );
            if ( ! dlk.got() ) {
                errmsg = "the collection's metadata lock is taken";
                result.append( "who" , dlk.other() );
                return false;
            }

            BSONObj chunkInfo = BSON("min" << min << "max" << max << "from" << fromShard.getName() << "to" << toShard.getName());
            configServer.logChange( "moveChunk.start" , ns , chunkInfo );

            ShardChunkVersion maxVersion;
            string myOldShard;
            {
                ScopedDbConnection conn( shardingState.getConfigServer() );

                BSONObj x = conn->findOne( ShardNS::chunk , Query( BSON( "ns" << ns ) ).sort( BSON( "lastmod" << -1 ) ) );
                maxVersion = x["lastmod"];

                BSONObj currChunk = conn->findOne( ShardNS::chunk , shardId.wrap( "_id" ) );
                assert( currChunk["shard"].type() );
                assert( currChunk["min"].type() );
                assert( currChunk["max"].type() );
                myOldShard = currChunk["shard"].String();
                conn.done();

                BSONObj currMin = currChunk["min"].Obj();
                BSONObj currMax = currChunk["max"].Obj();
                if ( currMin.woCompare( min ) || currMax.woCompare( max ) ) {
                    errmsg = "boundaries are outdated (likely a split occurred)";
                    result.append( "currMin" , currMin );
                    result.append( "currMax" , currMax );
                    result.append( "requestedMin" , min );
                    result.append( "requestedMax" , max );

                    log( LL_WARNING ) << "aborted moveChunk because" <<  errmsg << ": " << min << "->" << max
                                      << " is now " << currMin << "->" << currMax << endl;
                    return false;
                }

                if ( myOldShard != fromShard.getName() ) {
                    errmsg = "location is outdated (likely balance or migrate occurred)";
                    result.append( "from" , fromShard.getName() );
                    result.append( "official" , myOldShard );

                    log( LL_WARNING ) << "aborted moveChunk because " << errmsg << ": chunk is at " << myOldShard
                                      << " and not at " << fromShard.getName() << endl;
                    return false;
                }

                if ( maxVersion < shardingState.getVersion( ns ) ) {
                    errmsg = "official version less than mine?";
                    result.appendTimestamp( "officialVersion" , maxVersion );
                    result.appendTimestamp( "myVersion" , shardingState.getVersion( ns ) );

                    log( LL_WARNING ) << "aborted moveChunk because " << errmsg << ": official " << maxVersion
                                      << " mine: " << shardingState.getVersion(ns) << endl;
                    return false;
                }

                // since this could be the first call that enable sharding we also make sure to have the chunk manager up to date
                shardingState.gotShardName( myOldShard );
                ShardChunkVersion shardVersion;
                shardingState.trySetVersion( ns , shardVersion /* will return updated */ );

                log() << "moveChunk request accepted at version " << shardVersion << endl;
            }

            timing.done(2);

            // 3.
            MigrateStatusHolder statusHolder( ns , min , max );
            {
                // this gets a read lock, so we know we have a checkpoint for mods
                if ( ! migrateFromStatus.storeCurrentLocs( maxChunkSize , errmsg , result ) )
                    return false;

                ScopedDbConnection connTo( to );
                BSONObj res;
                bool ok = connTo->runCommand( "admin" ,
                                              BSON( "_recvChunkStart" << ns <<
                                                    "from" << from <<
                                                    "min" << min <<
                                                    "max" << max <<
                                                    "configServer" << configServer.modelServer()
                                                  ) ,
                                              res );
                connTo.done();

                if ( ! ok ) {
                    errmsg = "moveChunk failed to engage TO-shard in the data transfer: ";
                    assert( res["errmsg"].type() );
                    errmsg += res["errmsg"].String();
                    result.append( "cause" , res );
                    return false;
                }

            }
            timing.done( 3 );

            // 4.
            for ( int i=0; i<86400; i++ ) { // don't want a single chunk move to take more than a day
                assert( dbMutex.getState() == 0 );
                sleepsecs( 1 );
                ScopedDbConnection conn( to );
                BSONObj res;
                bool ok = conn->runCommand( "admin" , BSON( "_recvChunkStatus" << 1 ) , res );
                res = res.getOwned();
                conn.done();

                log(0) << "moveChunk data transfer progress: " << res << " my mem used: " << migrateFromStatus.mbUsed() << endl;

                if ( ! ok || res["state"].String() == "fail" ) {
                    log( LL_WARNING ) << "moveChunk error transfering data caused migration abort: " << res << endl;
                    errmsg = "data transfer error";
                    result.append( "cause" , res );
                    return false;
                }

                if ( res["state"].String() == "steady" )
                    break;

                if ( migrateFromStatus.mbUsed() > (500 * 1024 * 1024) ) {
                    // this is too much memory for us to use for this
                    // so we're going to abort the migrate
                    ScopedDbConnection conn( to );
                    BSONObj res;
                    conn->runCommand( "admin" , BSON( "_recvChunkAbort" << 1 ) , res );
                    res = res.getOwned();
                    conn.done();
                    error() << "aborting migrate because too much memory used res: " << res << endl;
                    errmsg = "aborting migrate because too much memory used";
                    result.appendBool( "split" , true );
                    return false;
                }

                killCurrentOp.checkForInterrupt();
            }
            timing.done(4);

            // 5.
            {
                // 5.a
                // we're under the collection lock here, so no other migrate can change maxVersion or ShardChunkManager state
                migrateFromStatus.setInCriticalSection( true );
                ShardChunkVersion currVersion = maxVersion;
                ShardChunkVersion myVersion = currVersion;
                myVersion.incMajor();

                {
                    writelock lk( ns );
                    assert( myVersion > shardingState.getVersion( ns ) );

                    // bump the chunks manager's version up and "forget" about the chunk being moved
                    // this is not the commit point but in practice the state in this shard won't until the commit it done
                    shardingState.donateChunk( ns , min , max , myVersion );
                }

                log() << "moveChunk setting version to: " << myVersion << endl;

                // 5.b
                // we're under the collection lock here, too, so we can undo the chunk donation because no other state change
                // could be ongoing
                {
                    BSONObj res;
                    ScopedDbConnection connTo( to );
                    bool ok = connTo->runCommand( "admin" ,
                                                  BSON( "_recvChunkCommit" << 1 ) ,
                                                  res );
                    connTo.done();

                    if ( ! ok ) {
                        {
                            writelock lk( ns );

                            // revert the chunk manager back to the state before "forgetting" about the chunk
                            shardingState.undoDonateChunk( ns , min , max , currVersion );
                        }

                        log() << "movChunk migrate commit not accepted by TO-shard: " << res
                              << " resetting shard version to: " << currVersion << endl;

                        errmsg = "_recvChunkCommit failed!";
                        result.append( "cause" , res );
                        return false;
                    }

                    log() << "moveChunk migrate commit accepted by TO-shard: " << res << endl;
                }

                // 5.c

                // version at which the next highest lastmod will be set
                // if the chunk being moved is the last in the shard, nextVersion is that chunk's lastmod
                // otherwise the highest version is from the chunk being bumped on the FROM-shard
                ShardChunkVersion nextVersion;

                // we want to go only once to the configDB but perhaps change two chunks, the one being migrated and another
                // local one (so to bump version for the entire shard)
                // we use the 'applyOps' mechanism to group the two updates and make them safer
                // TODO pull config update code to a module

                BSONObjBuilder cmdBuilder;

                BSONArrayBuilder updates( cmdBuilder.subarrayStart( "applyOps" ) );
                {
                    // update for the chunk being moved
                    BSONObjBuilder op;
                    op.append( "op" , "u" );
                    op.appendBool( "b" , false /* no upserting */ );
                    op.append( "ns" , ShardNS::chunk );

                    BSONObjBuilder n( op.subobjStart( "o" ) );
                    n.append( "_id" , Chunk::genID( ns , min ) );
                    n.appendTimestamp( "lastmod" , myVersion /* same as used on donateChunk */ );
                    n.append( "ns" , ns );
                    n.append( "min" , min );
                    n.append( "max" , max );
                    n.append( "shard" , toShard.getName() );
                    n.done();

                    BSONObjBuilder q( op.subobjStart( "o2" ) );
                    q.append( "_id" , Chunk::genID( ns , min ) );
                    q.done();

                    updates.append( op.obj() );
                }

                nextVersion = myVersion;

                // if we have chunks left on the FROM shard, update the version of one of them as well
                // we can figure that out by grabbing the chunkManager installed on 5.a
                // TODO expose that manager when installing it

                ShardChunkManagerPtr chunkManager = shardingState.getShardChunkManager( ns );
                if( chunkManager->getNumChunks() > 0 ) {

                    // get another chunk on that shard
                    BSONObj lookupKey;
                    BSONObj bumpMin, bumpMax;
                    do {
                        chunkManager->getNextChunk( lookupKey , &bumpMin , &bumpMax );
                        lookupKey = bumpMin;
                    }
                    while( bumpMin == min );

                    BSONObjBuilder op;
                    op.append( "op" , "u" );
                    op.appendBool( "b" , false );
                    op.append( "ns" , ShardNS::chunk );

                    nextVersion.incMinor();  // same as used on donateChunk
                    BSONObjBuilder n( op.subobjStart( "o" ) );
                    n.append( "_id" , Chunk::genID( ns , bumpMin ) );
                    n.appendTimestamp( "lastmod" , nextVersion );
                    n.append( "ns" , ns );
                    n.append( "min" , bumpMin );
                    n.append( "max" , bumpMax );
                    n.append( "shard" , fromShard.getName() );
                    n.done();

                    BSONObjBuilder q( op.subobjStart( "o2" ) );
                    q.append( "_id" , Chunk::genID( ns , bumpMin  ) );
                    q.done();

                    updates.append( op.obj() );

                    log() << "moveChunk updating self version to: " << nextVersion << " through "
                          << bumpMin << " -> " << bumpMax << " for collection '" << ns << "'" << endl;

                }
                else {

                    log() << "moveChunk moved last chunk out for collection '" << ns << "'" << endl;
                }

                updates.done();

                BSONArrayBuilder preCond( cmdBuilder.subarrayStart( "preCondition" ) );
                {
                    BSONObjBuilder b;
                    b.append( "ns" , ShardNS::chunk );
                    b.append( "q" , BSON( "query" << BSON( "ns" << ns ) << "orderby" << BSON( "lastmod" << -1 ) ) );
                    {
                        BSONObjBuilder bb( b.subobjStart( "res" ) );
                        bb.appendTimestamp( "lastmod" , maxVersion );
                        bb.done();
                    }
                    preCond.append( b.obj() );
                }

                preCond.done();

                BSONObj cmd = cmdBuilder.obj();
                log(7) << "moveChunk update: " << cmd << endl;

                bool ok = false;
                BSONObj cmdResult;
                try {
                    ScopedDbConnection conn( shardingState.getConfigServer() );
                    ok = conn->runCommand( "config" , cmd , cmdResult );
                    conn.done();
                }
                catch ( DBException& e ) {
                    ok = false;
                    BSONObjBuilder b;
                    e.getInfo().append( b );
                    cmdResult = b.obj();
                }

                if ( ! ok ) {

                    // this could be a blip in the connectivity
                    // wait out a few seconds and check if the commit request made it
                    //
                    // if the commit made it to the config, we'll see the chunk in the new shard and there's no action
                    // if the commit did not make it, currently the only way to fix this state is to bounce the mongod so
                    // that the old state (before migrating) be brought in

                    warning() << "moveChunk commit outcome ongoing: " << cmd << " for command :" << cmdResult << endl;
                    sleepsecs( 10 );

                    try {
                        ScopedDbConnection conn( shardingState.getConfigServer() );

                        // look for the chunk in this shard whose version got bumped
                        // we assume that if that mod made it to the config, the applyOps was successful
                        BSONObj doc = conn->findOne( ShardNS::chunk , Query(BSON( "ns" << ns )).sort( BSON("lastmod" << -1)));
                        ShardChunkVersion checkVersion = doc["lastmod"];

                        if ( checkVersion == nextVersion ) {
                            log() << "moveChunk commit confirmed" << endl;

                        }
                        else {
                            error() << "moveChunk commit failed: version is at"
                                            << checkVersion << " instead of " << nextVersion << endl;
                            error() << "TERMINATING" << endl;
                            dbexit( EXIT_SHARDING_ERROR );
                        }

                        conn.done();

                    }
                    catch ( ... ) {
                        error() << "moveChunk failed to get confirmation of commit" << endl;
                        error() << "TERMINATING" << endl;
                        dbexit( EXIT_SHARDING_ERROR );
                    }
                }

                migrateFromStatus.setInCriticalSection( false );

                // 5.d
                configServer.logChange( "moveChunk.commit" , ns , chunkInfo );
            }

            migrateFromStatus.done();
            timing.done(5);

            {
                // 6.
                OldDataCleanup c;
                c.ns = ns;
                c.min = min.getOwned();
                c.max = max.getOwned();
                ClientCursor::find( ns , c.initial );
                if ( c.initial.size() ) {
                    log() << "forking for cleaning up chunk data" << endl;
                    boost::thread t( boost::bind( &cleanupOldData , c ) );
                }
                else {
                    log() << "doing delete inline" << endl;
                    // 7.
                    c.doRemove();
                }


            }
            timing.done(6);

            return true;

        }

    } moveChunkCmd;

    bool ShardingState::inCriticalMigrateSection() {
        return migrateFromStatus.getInCriticalSection();
    }

    /* -----
       below this are the "to" side commands

       command to initiate
       worker thread
         does initial clone
         pulls initial change set
         keeps pulling
         keeps state
       command to get state
       commend to "commit"
    */

    class MigrateStatus {
    public:

        MigrateStatus() : m_active("MigrateStatus") { active = false; }

        void prepare() {
            scoped_lock l(m_active); // reading and writing 'active'

            assert( ! active );
            state = READY;
            errmsg = "";

            numCloned = 0;
            clonedBytes = 0;
            numCatchup = 0;
            numSteady = 0;

            active = true;
        }

        void go() {
            try {
                _go();
            }
            catch ( std::exception& e ) {
                state = FAIL;
                errmsg = e.what();
                log( LL_ERROR ) << "migrate failed: " << e.what() << endl;
            }
            catch ( ... ) {
                state = FAIL;
                errmsg = "UNKNOWN ERROR";
                log( LL_ERROR ) << "migrate failed with unknown exception" << endl;
            }
            setActive( false );
        }

        void _go() {
            assert( getActive() );
            assert( state == READY );
            assert( ! min.isEmpty() );
            assert( ! max.isEmpty() );
            
            slaveCount = ( getSlaveCount() / 2 ) + 1;

            MoveTimingHelper timing( "to" , ns , min , max , 5 /* steps */ );

            ScopedDbConnection conn( from );
            conn->getLastError(); // just test connection

            {
                // 1. copy indexes
                auto_ptr<DBClientCursor> indexes = conn->getIndexes( ns );
                vector<BSONObj> all;
                while ( indexes->more() ) {
                    all.push_back( indexes->next().getOwned() );
                }

                writelock lk( ns );
                Client::Context ct( ns );

                string system_indexes = cc().database()->name + ".system.indexes";
                for ( unsigned i=0; i<all.size(); i++ ) {
                    BSONObj idx = all[i];
                    theDataFileMgr.insertAndLog( system_indexes.c_str() , idx );
                }

                timing.done(1);
            }

            {
                // 2. delete any data already in range
                writelock lk( ns );
                RemoveSaver rs( "moveChunk" , ns , "preCleanup" );
                long long num = Helpers::removeRange( ns , min , max , true , false , cmdLine.moveParanoia ? &rs : 0 );
                if ( num )
                    log( LL_WARNING ) << "moveChunkCmd deleted data already in chunk # objects: " << num << endl;

                timing.done(2);
            }


            {
                // 3. initial bulk clone
                state = CLONE;

                while ( true ) {
                    BSONObj res;
                    if ( ! conn->runCommand( "admin" , BSON( "_migrateClone" << 1 ) , res ) ) {
                        state = FAIL;
                        errmsg = "_migrateClone failed: ";
                        errmsg += res.toString();
                        error() << errmsg << endl;
                        conn.done();
                        return;
                    }

                    BSONObj arr = res["objects"].Obj();
                    int thisTime = 0;

                    BSONObjIterator i( arr );
                    while( i.more() ) {
                        BSONObj o = i.next().Obj();
                        {
                            writelock lk( ns );
                            Helpers::upsert( ns , o );
                        }
                        thisTime++;
                        numCloned++;
                        clonedBytes += o.objsize();
                    }

                    if ( thisTime == 0 )
                        break;
                }

                timing.done(3);
            }

            // if running on a replicated system, we'll need to flush the docs we cloned to the secondaries
            ReplTime lastOpApplied;

            {
                // 4. do bulk of mods
                state = CATCHUP;
                while ( true ) {
                    BSONObj res;
                    if ( ! conn->runCommand( "admin" , BSON( "_transferMods" << 1 ) , res ) ) {
                        state = FAIL;
                        errmsg = "_transferMods failed: ";
                        errmsg += res.toString();
                        log( LL_ERROR ) << "_transferMods failed: " << res << endl;
                        conn.done();
                        return;
                    }
                    if ( res["size"].number() == 0 )
                        break;

                    apply( res , &lastOpApplied );
                    
                    const int maxIterations = 3600*50;
                    int i;
                    for ( i=0;i<maxIterations; i++) {
                        if ( state == ABORT ) {
                            timing.note( "aborted" );
                            return;
                        }
                        
                        if ( opReplicatedEnough( lastOpApplied ) )
                            break;
                        
                        if ( i > 100 ) {
                            warning() << "secondaries having hard time keeping up with migrate" << endl;
                        }

                        sleepmillis( 20 );
                    }

                    if ( i == maxIterations ) {
                        errmsg = "secondary can't keep up with migrate";
                        error() << errmsg << endl;
                        conn.done();
                        state = FAIL;
                        return;
                    } 
                }

                timing.done(4);
            }

            {
                // 5. wait for commit
                Timer timeWaitingForCommit;

                state = STEADY;
                while ( state == STEADY || state == COMMIT_START ) {
                    BSONObj res;
                    if ( ! conn->runCommand( "admin" , BSON( "_transferMods" << 1 ) , res ) ) {
                        log() << "_transferMods failed in STEADY state: " << res << endl;
                        errmsg = res.toString();
                        state = FAIL;
                        conn.done();
                        return;
                    }

                    if ( res["size"].number() > 0 && apply( res , &lastOpApplied ) )
                        continue;

                    if ( state == COMMIT_START && flushPendingWrites( lastOpApplied ) )
                        break;

                    sleepmillis( 10 );
                }

                if ( state == ABORT ) {
                    timing.note( "aborted" );
                    return;
                }

                if ( timeWaitingForCommit.seconds() > 86400 ) {
                    state = FAIL;
                    errmsg = "timed out waiting for commit";
                    return;
                }

                timing.done(5);
            }

            state = DONE;
            conn.done();
        }

        void status( BSONObjBuilder& b ) {
            b.appendBool( "active" , getActive() );

            b.append( "ns" , ns );
            b.append( "from" , from );
            b.append( "min" , min );
            b.append( "max" , max );

            b.append( "state" , stateString() );
            if ( state == FAIL )
                b.append( "errmsg" , errmsg );
            {
                BSONObjBuilder bb( b.subobjStart( "counts" ) );
                bb.append( "cloned" , numCloned );
                bb.append( "clonedBytes" , clonedBytes );
                bb.append( "catchup" , numCatchup );
                bb.append( "steady" , numSteady );
                bb.done();
            }


        }

        bool apply( const BSONObj& xfer , ReplTime* lastOpApplied ) {
            ReplTime dummy;
            if ( lastOpApplied == NULL ) {
                lastOpApplied = &dummy;
            }

            bool didAnything = false;

            if ( xfer["deleted"].isABSONObj() ) {
                writelock lk(ns);
                Client::Context cx(ns);

                RemoveSaver rs( "moveChunk" , ns , "removedDuring" );

                BSONObjIterator i( xfer["deleted"].Obj() );
                while ( i.more() ) {
                    BSONObj id = i.next().Obj();

                    // do not apply deletes if they do not belong to the chunk being migrated
                    BSONObj fullObj;
                    if ( Helpers::findById( cc() , ns.c_str() , id, fullObj ) ) {
                        if ( ! isInRange( fullObj , min , max ) ) {
                            log() << "not applying out of range deletion: " << fullObj << endl;

                            continue;
                        }
                    }

                    Helpers::removeRange( ns , id , id, false , true , cmdLine.moveParanoia ? &rs : 0 );

                    *lastOpApplied = cx.getClient()->getLastOp();
                    didAnything = true;
                }
            }

            if ( xfer["reload"].isABSONObj() ) {
                writelock lk(ns);
                Client::Context cx(ns);

                BSONObjIterator i( xfer["reload"].Obj() );
                while ( i.more() ) {
                    BSONObj it = i.next().Obj();

                    Helpers::upsert( ns , it );

                    *lastOpApplied = cx.getClient()->getLastOp();
                    didAnything = true;
                }
            }

            return didAnything;
        }

        bool opReplicatedEnough( const ReplTime& lastOpApplied ) {
            // if replication is on, try to force enough secondaries to catch up
            // TODO opReplicatedEnough should eventually honor priorities and geo-awareness
            //      for now, we try to replicate to a sensible number of secondaries
            return mongo::opReplicatedEnough( lastOpApplied , slaveCount );
        }

        bool flushPendingWrites( const ReplTime& lastOpApplied ) {
            if ( ! opReplicatedEnough( lastOpApplied ) ) {
                warning() << "migrate commit attempt timed out contacting " << slaveCount
                          << " slaves for '" << ns << "' " << min << " -> " << max << endl;
                return false;
            }
            log() << "migrate commit succeeded flushing to secondaries for '" << ns << "' " << min << " -> " << max << endl;

            {
                readlock lk(ns);  // commitNow() currently requires it

                // if durability is on, force a write to journal
                if ( getDur().commitNow() ) {
                    log() << "migrate commit flushed to journal for '" << ns << "' " << min << " -> " << max << endl;
                }
            }

            return true;
        }

        string stateString() {
            switch ( state ) {
            case READY: return "ready";
            case CLONE: return "clone";
            case CATCHUP: return "catchup";
            case STEADY: return "steady";
            case COMMIT_START: return "commitStart";
            case DONE: return "done";
            case FAIL: return "fail";
            case ABORT: return "abort";
            }
            assert(0);
            return "";
        }

        bool startCommit() {
            if ( state != STEADY )
                return false;
            state = COMMIT_START;

            for ( int i=0; i<86400; i++ ) {
                sleepmillis(1);
                if ( state == DONE )
                    return true;
            }
            log() << "startCommit never finished!" << endl;
            return false;
        }

        void abort() {
            state = ABORT;
            errmsg = "aborted";
        }

        bool getActive() const { scoped_lock l(m_active); return active; }
        void setActive( bool b ) { scoped_lock l(m_active); active = b; }

        mutable mongo::mutex m_active;
        bool active;

        string ns;
        string from;

        BSONObj min;
        BSONObj max;

        long long numCloned;
        long long clonedBytes;
        long long numCatchup;
        long long numSteady;
        
        int slaveCount;

        enum State { READY , CLONE , CATCHUP , STEADY , COMMIT_START , DONE , FAIL , ABORT } state;
        string errmsg;

    } migrateStatus;

    void migrateThread() {
        Client::initThread( "migrateThread" );
        migrateStatus.go();
        cc().shutdown();
    }

    class RecvChunkStartCommand : public ChunkCommandHelper {
    public:
        RecvChunkStartCommand() : ChunkCommandHelper( "_recvChunkStart" ) {}

        virtual LockType locktype() const { return WRITE; }  // this is so don't have to do locking internally

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {

            if ( migrateStatus.getActive() ) {
                errmsg = "migrate already in progress";
                return false;
            }
            
            if ( OldDataCleanup::_numThreads > 0 ) {
                errmsg = 
                    str::stream() 
                    << "still waiting for a previous migrates data to get cleaned, can't accept new chunks, num threads: " 
                    << OldDataCleanup::_numThreads;
                return false;
            }

            if ( ! configServer.ok() )
                configServer.init( cmdObj["configServer"].String() );

            migrateStatus.prepare();

            migrateStatus.ns = cmdObj.firstElement().String();
            migrateStatus.from = cmdObj["from"].String();
            migrateStatus.min = cmdObj["min"].Obj().getOwned();
            migrateStatus.max = cmdObj["max"].Obj().getOwned();

            boost::thread m( migrateThread );

            result.appendBool( "started" , true );
            return true;
        }

    } recvChunkStartCmd;

    class RecvChunkStatusCommand : public ChunkCommandHelper {
    public:
        RecvChunkStatusCommand() : ChunkCommandHelper( "_recvChunkStatus" ) {}

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            migrateStatus.status( result );
            return 1;
        }

    } recvChunkStatusCommand;

    class RecvChunkCommitCommand : public ChunkCommandHelper {
    public:
        RecvChunkCommitCommand() : ChunkCommandHelper( "_recvChunkCommit" ) {}

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            bool ok = migrateStatus.startCommit();
            migrateStatus.status( result );
            return ok;
        }

    } recvChunkCommitCommand;

    class RecvChunkAbortCommand : public ChunkCommandHelper {
    public:
        RecvChunkAbortCommand() : ChunkCommandHelper( "_recvChunkAbort" ) {}

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            migrateStatus.abort();
            migrateStatus.status( result );
            return true;
        }

    } recvChunkAboortCommand;


    class IsInRangeTest : public UnitTest {
    public:
        void run() {
            BSONObj min = BSON( "x" << 1 );
            BSONObj max = BSON( "x" << 5 );

            assert( ! isInRange( BSON( "x" << 0 ) , min , max ) );
            assert( isInRange( BSON( "x" << 1 ) , min , max ) );
            assert( isInRange( BSON( "x" << 3 ) , min , max ) );
            assert( isInRange( BSON( "x" << 4 ) , min , max ) );
            assert( ! isInRange( BSON( "x" << 5 ) , min , max ) );
            assert( ! isInRange( BSON( "x" << 6 ) , min , max ) );

            log(1) << "isInRangeTest passed" << endl;
        }
    } isInRangeTest;
}
