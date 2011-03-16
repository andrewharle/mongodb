/** @file connpool.h */

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <stack>
#include "dbclient.h"
#include "redef_macros.h"

namespace mongo {

    class Shard;

    /**
     * not thread safe
     * thread safety is handled by DBConnectionPool
     */
    class PoolForHost {
    public:
        PoolForHost()
            : _created(0) {}

        PoolForHost( const PoolForHost& other ) {
            assert(other._pool.size() == 0);
            _created = other._created;
            assert( _created == 0 );
        }

        ~PoolForHost();

        int numAvailable() const { return (int)_pool.size(); }

        void createdOne( DBClientBase * base);
        long long numCreated() const { return _created; }

        ConnectionString::ConnectionType type() const { assert(_created); return _type; }

        /**
         * gets a connection or return NULL
         */
        DBClientBase * get();

        void done( DBClientBase * c );

        void flush();

        static void setMaxPerHost( unsigned max ) { _maxPerHost = max; }
        static unsigned getMaxPerHost() { return _maxPerHost; }
    private:

        struct StoredConnection {
            StoredConnection( DBClientBase * c );

            bool ok( time_t now );

            DBClientBase* conn;
            time_t when;
        };

        std::stack<StoredConnection> _pool;
        long long _created;
        ConnectionString::ConnectionType _type;

        static unsigned _maxPerHost;
    };

    class DBConnectionHook {
    public:
        virtual ~DBConnectionHook() {}
        virtual void onCreate( DBClientBase * conn ) {}
        virtual void onHandedOut( DBClientBase * conn ) {}
    };

    /** Database connection pool.

        Generally, use ScopedDbConnection and do not call these directly.

        This class, so far, is suitable for use with unauthenticated connections.
        Support for authenticated connections requires some adjustements: please
        request...

        Usage:

        {
           ScopedDbConnection c("myserver");
           c.conn()...
        }
    */
    class DBConnectionPool {
        
    public:

        /** compares server namees, but is smart about replica set names */
        struct serverNameCompare {
            bool operator()( const string& a , const string& b ) const;
        };

    private:

        mongo::mutex _mutex;
        typedef map<string,PoolForHost,serverNameCompare> PoolMap; // servername -> pool
        PoolMap _pools;
        list<DBConnectionHook*> _hooks;
        string _name;

        DBClientBase* _get( const string& ident );

        DBClientBase* _finishCreate( const string& ident , DBClientBase* conn );

    public:
        DBConnectionPool() : _mutex("DBConnectionPool") , _name( "dbconnectionpool" ) { }
        ~DBConnectionPool();

        /** right now just controls some asserts.  defaults to "dbconnectionpool" */
        void setName( const string& name ) { _name = name; }

        void onCreate( DBClientBase * conn );
        void onHandedOut( DBClientBase * conn );

        void flush();

        DBClientBase *get(const string& host);
        DBClientBase *get(const ConnectionString& host);

        void release(const string& host, DBClientBase *c) {
            if ( c->isFailed() ) {
                delete c;
                return;
            }
            scoped_lock L(_mutex);
            _pools[host].done(c);
        }
        void addHook( DBConnectionHook * hook );
        void appendInfo( BSONObjBuilder& b );
    };

    extern DBConnectionPool pool;

    class AScopedConnection : boost::noncopyable {
    public:
        AScopedConnection() { _numConnections++; }
        virtual ~AScopedConnection() { _numConnections--; }
        virtual DBClientBase* get() = 0;
        virtual void done() = 0;
        virtual string getHost() const = 0;

        /**
         * @return total number of current instances of AScopedConnection
         */
        static int getNumConnections() { return _numConnections; }

    private:
        static AtomicUInt _numConnections;
    };

    /** Use to get a connection from the pool.  On exceptions things
       clean up nicely (i.e. the socket gets closed automatically when the
       scopeddbconnection goes out of scope).
    */
    class ScopedDbConnection : public AScopedConnection {
    public:
        /** the main constructor you want to use
            throws UserException if can't connect
            */
        explicit ScopedDbConnection(const string& host) : _host(host), _conn( pool.get(host) ) {}

        ScopedDbConnection() : _host( "" ) , _conn(0) {}

        /* @param conn - bind to an existing connection */
        ScopedDbConnection(const string& host, DBClientBase* conn ) : _host( host ) , _conn( conn ) {}

        /** throws UserException if can't connect */
        explicit ScopedDbConnection(const ConnectionString& url ) : _host(url.toString()), _conn( pool.get(url) ) {}

        /** throws UserException if can't connect */
        explicit ScopedDbConnection(const Shard& shard );
        explicit ScopedDbConnection(const Shard* shard );

        ~ScopedDbConnection();

        /** get the associated connection object */
        DBClientBase* operator->() {
            uassert( 11004 ,  "connection was returned to the pool already" , _conn );
            return _conn;
        }

        /** get the associated connection object */
        DBClientBase& conn() {
            uassert( 11005 ,  "connection was returned to the pool already" , _conn );
            return *_conn;
        }

        /** get the associated connection object */
        DBClientBase* get() {
            uassert( 13102 ,  "connection was returned to the pool already" , _conn );
            return _conn;
        }

        string getHost() const { return _host; }

        /** Force closure of the connection.  You should call this if you leave it in
            a bad state.  Destructor will do this too, but it is verbose.
        */
        void kill() {
            delete _conn;
            _conn = 0;
        }

        /** Call this when you are done with the connection.

            If you do not call done() before this object goes out of scope,
            we can't be sure we fully read all expected data of a reply on the socket.  so
            we don't try to reuse the connection in that situation.
        */
        void done() {
            if ( ! _conn )
                return;

            /* we could do this, but instead of assume one is using autoreconnect mode on the connection
            if ( _conn->isFailed() )
                kill();
            else
            */
            pool.release(_host, _conn);
            _conn = 0;
        }

        ScopedDbConnection * steal();

    private:
        const string _host;
        DBClientBase *_conn;

    };

} // namespace mongo

#include "undef_macros.h"
