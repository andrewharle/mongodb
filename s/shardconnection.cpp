// shardconnection.cpp

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

#include "pch.h"
#include "shard.h"
#include "config.h"
#include "request.h"
#include <set>

namespace mongo {

    // The code in shardconnection may run not only in mongos context. When elsewhere, chunk shard versioning
    // is disabled. To enable chunk shard versioning, provide the check/resetShardVerionCB's below
    //
    // TODO: better encapsulate this mechanism.

    bool defaultIsVersionable( DBClientBase * conn ){
        return false;
    }

    bool defaultInitShardVersion( DBClientBase & conn, BSONObj& result ){
        return false;
    }

    bool defaultCheckShardVersion( DBClientBase & conn , const string& ns , bool authoritative , int tryNumber ) {
        // no-op in mongod
        return false;
    }

    void defaultResetShardVersion( DBClientBase * conn ) {
        // no-op in mongod
    }

    boost::function1<bool, DBClientBase* > isVersionableCB = defaultIsVersionable;
    boost::function2<bool, DBClientBase&, BSONObj& > initShardVersionCB = defaultInitShardVersion;
    boost::function4<bool, DBClientBase&, const string&, bool, int> checkShardVersionCB = defaultCheckShardVersion;
    boost::function1<void, DBClientBase*> resetShardVersionCB = defaultResetShardVersion;

    DBConnectionPool shardConnectionPool;

    /**
     * holds all the actual db connections for a client to various servers
     * 1 per thread, so doesn't have to be thread safe
     */
    class ClientConnections : boost::noncopyable {
    public:
        struct Status : boost::noncopyable {
            Status() : created(0), avail(0) {}

            long long created;
            DBClientBase* avail;
        };


        ClientConnections() {}

        ~ClientConnections() {
            for ( HostMap::iterator i=_hosts.begin(); i!=_hosts.end(); ++i ) {
                string addr = i->first;
                Status* ss = i->second;
                assert( ss );
                if ( ss->avail ) {
                    /* if we're shutting down, don't want to initiate release mechanism as it is slow,
                       and isn't needed since all connections will be closed anyway */
                    if ( inShutdown() ) {
                        if( isVersionableCB( ss->avail ) ) resetShardVersionCB( ss->avail );
                        delete ss->avail;
                    }
                    else
                        release( addr , ss->avail );
                    ss->avail = 0;
                }
                delete ss;
            }
            _hosts.clear();
        }

        DBClientBase * get( const string& addr , const string& ns ) {
            _check( ns );

            Status* &s = _hosts[addr];
            if ( ! s )
                s = new Status();

            if ( s->avail ) {
                DBClientBase* c = s->avail;
                s->avail = 0;
                try {
                    shardConnectionPool.onHandedOut( c );
                }
                catch ( std::exception& e ) {
                    delete c;
                    throw;
                }
                return c;
            }

            s->created++;
            return shardConnectionPool.get( addr );
        }

        void done( const string& addr , DBClientBase* conn ) {
            Status* s = _hosts[addr];
            assert( s );
            if ( s->avail ) {
                release( addr , conn );
                return;
            }
            s->avail = conn;
        }

        void sync() {
            for ( HostMap::iterator i=_hosts.begin(); i!=_hosts.end(); ++i ) {
                string addr = i->first;
                Status* ss = i->second;
                if ( ss->avail )
                    ss->avail->getLastError();
                
            }
        }

        void checkVersions( const string& ns ) {

            vector<Shard> all;
            Shard::getAllShards( all );

            // Now only check top-level shard connections
            for ( unsigned i=0; i<all.size(); i++ ) {

                string sconnString = all[i].getConnString();
                Status* &s = _hosts[sconnString];

                if ( ! s ){
                    s = new Status();
                }

                if( ! s->avail )
                    s->avail = shardConnectionPool.get( sconnString );

                checkShardVersionCB( *s->avail, ns, false, 1 );

            }
        }

        void release( const string& addr , DBClientBase * conn ) {
            shardConnectionPool.release( addr , conn );
        }

        void _check( const string& ns ) {
            if ( ns.size() == 0 || _seenNS.count( ns ) )
                return;
            _seenNS.insert( ns );
            checkVersions( ns );
        }
        
        typedef map<string,Status*,DBConnectionPool::serverNameCompare> HostMap;
        HostMap _hosts;
        set<string> _seenNS;
        // -----

        static thread_specific_ptr<ClientConnections> _perThread;

        static ClientConnections* threadInstance() {
            ClientConnections* cc = _perThread.get();
            if ( ! cc ) {
                cc = new ClientConnections();
                _perThread.reset( cc );
            }
            return cc;
        }
    };

    thread_specific_ptr<ClientConnections> ClientConnections::_perThread;

    ShardConnection::ShardConnection( const Shard * s , const string& ns )
        : _addr( s->getConnString() ) , _ns( ns ) {
        _init();
    }

    ShardConnection::ShardConnection( const Shard& s , const string& ns )
        : _addr( s.getConnString() ) , _ns( ns ) {
        _init();
    }

    ShardConnection::ShardConnection( const string& addr , const string& ns )
        : _addr( addr ) , _ns( ns ) {
        _init();
    }

    void ShardConnection::_init() {
        assert( _addr.size() );
        _conn = ClientConnections::threadInstance()->get( _addr , _ns );
        _finishedInit = false;
    }

    void ShardConnection::_finishInit() {
        if ( _finishedInit )
            return;
        _finishedInit = true;

        if ( _ns.size() && isVersionableCB( _conn ) ) {
            _setVersion = checkShardVersionCB( *_conn , _ns , false , 1 );
        }
        else {
            _setVersion = false;
        }

    }

    void ShardConnection::done() {
        if ( _conn ) {
            ClientConnections::threadInstance()->done( _addr , _conn );
            _conn = 0;
            _finishedInit = true;
        }
    }

    void ShardConnection::kill() {
        if ( _conn ) {
            if( isVersionableCB( _conn ) ) resetShardVersionCB( _conn );
            delete _conn;
            _conn = 0;
            _finishedInit = true;
        }
    }

    void ShardConnection::sync() {
        ClientConnections::threadInstance()->sync();
    }

    bool ShardConnection::runCommand( const string& db , const BSONObj& cmd , BSONObj& res ) {
        assert( _conn );
        bool ok = _conn->runCommand( db , cmd , res );
        if ( ! ok ) {
            if ( res["code"].numberInt() == StaleConfigInContextCode ) {
                string big = res["errmsg"].String();
                string ns,raw;
                massert( 13409 , (string)"can't parse ns from: " + big  , StaleConfigException::parse( big , ns , raw ) );
                done();
                throw StaleConfigException( ns , raw );
            }
        }
        return ok;
    }

    void ShardConnection::checkMyConnectionVersions( const string & ns ) {
        ClientConnections::threadInstance()->checkVersions( ns );
    }

    ShardConnection::~ShardConnection() {
        if ( _conn ) {
            if ( ! _conn->isFailed() ) {
                /* see done() comments above for why we log this line */
                log() << "sharded connection to " << _conn->getServerAddress() << " not being returned to the pool" << endl;
            }
            kill();
        }
    }
}
