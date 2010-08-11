// cursors.cpp
/*
 *    Copyright (C) 2010 10gen Inc.
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


#include "pch.h"
#include "cursors.h"
#include "../client/connpool.h"
#include "../db/queryutil.h"
#include "../db/commands.h"
#include "../util/background.h"

namespace mongo {
    
    // --------  ShardedCursor -----------

    ShardedClientCursor::ShardedClientCursor( QueryMessage& q , ClusteredCursor * cursor ){
        assert( cursor );
        _cursor = cursor;
        
        _skip = q.ntoskip;
        _ntoreturn = q.ntoreturn;
        
        _totalSent = 0;
        _done = false;

        _id = 0;
        
        if ( q.queryOptions & QueryOption_NoCursorTimeout ){
            _lastAccessMillis = 0;
        }
        else 
            _lastAccessMillis = Listener::getElapsedTimeMillis();
    }

    ShardedClientCursor::~ShardedClientCursor(){
        assert( _cursor );
        delete _cursor;
        _cursor = 0;
    }

    long long ShardedClientCursor::getId(){
        if ( _id <= 0 ){
            _id = cursorCache.genId();
            assert( _id >= 0 );
        }
        return _id;
    }

    void ShardedClientCursor::accessed(){
        if ( _lastAccessMillis > 0 )
            _lastAccessMillis = Listener::getElapsedTimeMillis();
    }

    long long ShardedClientCursor::idleTime( long long now ){
        if ( _lastAccessMillis == 0 )
            return 0;
        return now - _lastAccessMillis;
    }

    bool ShardedClientCursor::sendNextBatch( Request& r , int ntoreturn ){
        uassert( 10191 ,  "cursor already done" , ! _done );
                
        int maxSize = 1024 * 1024;
        if ( _totalSent > 0 )
            maxSize *= 3;
        
        BufBuilder b(32768);
        
        int num = 0;
        bool sendMore = true;

        while ( _cursor->more() ){
            BSONObj o = _cursor->next();

            b.appendBuf( (void*)o.objdata() , o.objsize() );
            num++;
            
            if ( b.len() > maxSize ){
                break;
            }

            if ( num == ntoreturn ){
                // soft limit aka batch size
                break;
            }

            if ( ntoreturn != 0 && ( -1 * num + _totalSent ) == ntoreturn ){
                // hard limit - total to send
                sendMore = false;
                break;
            }

            if ( ntoreturn == 0 && _totalSent == 0 && num > 100 ){
                // first batch should be max 100 unless batch size specified
                break;
            }
        }

        bool hasMore = sendMore && _cursor->more();
        log(6) << "\t hasMore:" << hasMore << " wouldSendMoreIfHad: " << sendMore << " id:" << getId() << " totalSent: " << _totalSent << endl;
        
        replyToQuery( 0 , r.p() , r.m() , b.buf() , b.len() , num , _totalSent , hasMore ? getId() : 0 );
        _totalSent += num;
        _done = ! hasMore;
        
        return hasMore;
    }

    // ---- CursorCache -----
    
    long long CursorCache::TIMEOUT = 600000;

    CursorCache::CursorCache()
        :_mutex( "CursorCache" ), _shardedTotal(0){
    }

    CursorCache::~CursorCache(){
        // TODO: delete old cursors?
        int logLevel = 1;
        if ( _cursors.size() || _refs.size() )
            logLevel = 0;
        log( logLevel ) << " CursorCache at shutdown - "
                        << " sharded: " << _cursors.size() 
                        << " passthrough: " << _refs.size()
                        << endl;
    }

    ShardedClientCursorPtr CursorCache::get( long long id ){
        scoped_lock lk( _mutex );
        MapSharded::iterator i = _cursors.find( id );
        if ( i == _cursors.end() ){
            OCCASIONALLY log() << "Sharded CursorCache missing cursor id: " << id << endl;
            return ShardedClientCursorPtr();
        }
        i->second->accessed();
        return i->second;
    }
    
    void CursorCache::store( ShardedClientCursorPtr cursor ){
        assert( cursor->getId() );
        scoped_lock lk( _mutex );
        _cursors[cursor->getId()] = cursor;
        _shardedTotal++;
    }
    void CursorCache::remove( long long id ){
        assert( id );
        scoped_lock lk( _mutex );
        _cursors.erase( id );
    }

    void CursorCache::storeRef( const string& server , long long id ){
        assert( id );
        scoped_lock lk( _mutex );
        _refs[id] = server;
    }
    
    long long CursorCache::genId(){
        while ( true ){
            long long x = security.getNonce();
            if ( x == 0 )
                continue;
            if ( x < 0 )
                x *= -1;
            
            scoped_lock lk( _mutex );
            MapSharded::iterator i = _cursors.find( x );
            if ( i != _cursors.end() )
                continue;
            
            MapNormal::iterator j = _refs.find( x );
            if ( j != _refs.end() )
                continue;
            
            return x;
        }
    }

    void CursorCache::gotKillCursors(Message& m ){
        int *x = (int *) m.singleData()->_data;
        x++; // reserved
        int n = *x++;

        if ( n > 2000 ){
            log( n < 30000 ? LL_WARNING : LL_ERROR ) << "receivedKillCursors, n=" << n << endl;
        }


        uassert( 13286 , "sent 0 cursors to kill" , n >= 1 );
        uassert( 13287 , "too many cursors to kill" , n < 30000 );
        
        long long * cursors = (long long *)x;
        for ( int i=0; i<n; i++ ){
            long long id = cursors[i];
            if ( ! id ){
                log( LL_WARNING ) << " got cursor id of 0 to kill" << endl;
                continue;
            }
            
            string server;            
            {
                scoped_lock lk( _mutex );

                MapSharded::iterator i = _cursors.find( id );
                if ( i != _cursors.end() ){
                    _cursors.erase( i );
                    continue;
                }
                
                MapNormal::iterator j = _refs.find( id );
                if ( j == _refs.end() ){
                    log( LL_WARNING ) << "can't find cursor: " << id << endl;
                    continue;
                }
                server = j->second;
                _refs.erase( j );
            }
            
            assert( server.size() );
            ScopedDbConnection conn( server );
            conn->killCursor( id );
            conn.done();
        }
    }

    void CursorCache::appendInfo( BSONObjBuilder& result ){
        scoped_lock lk( _mutex );
        result.append( "sharded" , (int)_cursors.size() );
        result.appendNumber( "shardedEver" , _shardedTotal );
        result.append( "refs" , (int)_refs.size() );
        result.append( "totalOpen" , (int)(_cursors.size() + _refs.size() ) );
    }

    void CursorCache::doTimeouts(){
        long long now = Listener::getElapsedTimeMillis();
        scoped_lock lk( _mutex );
        for ( MapSharded::iterator i=_cursors.begin(); i!=_cursors.end(); ++i ){
            long long idleFor = i->second->idleTime( now );
            if ( idleFor < TIMEOUT ){
                continue;
            }
            log() << "killing old cursor " << i->second->getId() << " idle for: " << idleFor << "ms" << endl; // TODO: make log(1)
            _cursors.erase( i );
        }
    }

    CursorCache cursorCache;
    
    class CursorTimeoutThread : public PeriodicBackgroundJob {
    public:
        CursorTimeoutThread() : PeriodicBackgroundJob( 4000 ){}
        virtual string name() { return "cursorTimeout"; }
        virtual void runLoop(){
            cursorCache.doTimeouts();
        }
    } cursorTimeoutThread;

    void CursorCache::startTimeoutThread(){
        cursorTimeoutThread.go();
    }

    class CmdCursorInfo : public Command {
    public:
        CmdCursorInfo() : Command( "cursorInfo", true ) {}
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream& help ) const {
            help << " example: { cursorInfo : 1 }";
        }
        virtual LockType locktype() const { return NONE; }
        bool run(const string&, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            cursorCache.appendInfo( result );
            if ( jsobj["setTimeout"].isNumber() )
                CursorCache::TIMEOUT = jsobj["setTimeout"].numberLong();
            return true;
        }
    } cmdCursorInfo;

}
