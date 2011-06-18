/** @file dbclient_rs.h - connect to a Replica Set, from C++ */

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

#include "../pch.h"
#include "dbclient.h"

namespace mongo {

    class ReplicaSetMonitor;
    typedef shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorPtr;

    /**
     * manages state about a replica set for client
     * keeps tabs on whose master and what slaves are up
     * can hand a slave to someone for SLAVE_OK
     * one instace per process per replica set
     * TODO: we might be able to use a regular Node * to avoid _lock
     */
    class ReplicaSetMonitor {
    public:

        typedef boost::function1<void,const ReplicaSetMonitor*> ConfigChangeHook;

        /**
         * gets a cached Monitor per name or will create if doesn't exist
         */
        static ReplicaSetMonitorPtr get( const string& name , const vector<HostAndPort>& servers );

        /**
         * checks all sets for current master and new secondaries
         * usually only called from a BackgroundJob
         */
        static void checkAll();

        /**
         * this is called whenever the config of any repclia set changes
         * currently only 1 globally
         * asserts if one already exists
         * ownership passes to ReplicaSetMonitor and the hook will actually never be deleted
         */
        static void setConfigChangeHook( ConfigChangeHook hook );

        ~ReplicaSetMonitor();

        /** @return HostAndPort or throws an exception */
        HostAndPort getMaster();

        /**
         * notify the monitor that server has faild
         */
        void notifyFailure( const HostAndPort& server );

        /** @return prev if its still ok, and if not returns a random slave that is ok for reads */
        HostAndPort getSlave( const HostAndPort& prev );

        /** @return a random slave that is ok for reads */
        HostAndPort getSlave();


        /**
         * notify the monitor that server has faild
         */
        void notifySlaveFailure( const HostAndPort& server );

        /**
         * checks for current master and new secondaries
         */
        void check();

        string getName() const { return _name; }

        string getServerAddress() const;
        
        bool contains( const string& server ) const;

    private:
        /**
         * This populates a list of hosts from the list of seeds (discarding the
         * seed list).
         * @param name set name
         * @param servers seeds
         */
        ReplicaSetMonitor( const string& name , const vector<HostAndPort>& servers );

        void _check();

        /**
         * Use replSetGetStatus command to make sure hosts in host list are up
         * and readable.  Sets Node::ok appropriately.
         */
        void _checkStatus(DBClientConnection *conn);

        /**
         * Add array of hosts to host list. Doesn't do anything if hosts are
         * already in host list.
         * @param hostList the list of hosts to add
         * @param changed if new hosts were added
         */
        void _checkHosts(const BSONObj& hostList, bool& changed);

        /**
         * Updates host list.
         * @param c the connection to check
         * @param maybePrimary OUT
         * @param verbose
         * @return if the connection is good
         */
        bool _checkConnection( DBClientConnection * c , string& maybePrimary , bool verbose );

        int _find( const string& server ) const ;
        int _find_inlock( const string& server ) const ;
        int _find( const HostAndPort& server ) const ;

        mutable mongo::mutex _lock; // protects _nodes
        mutable mongo::mutex  _checkConnectionLock;

        string _name;
        struct Node {
            Node( const HostAndPort& a , DBClientConnection* c ) : addr( a ) , conn(c) , ok(true) {}
            HostAndPort addr;
            DBClientConnection* conn;

            // if this node is in a failure state
            // used for slave routing
            // this is too simple, should make it better
            bool ok;
        };

        /**
         * Host list.
         */
        vector<Node> _nodes;

        int _master; // which node is the current master.  -1 means no master is known
        int _nextSlave; // which node is the current slave

        static mongo::mutex _setsLock; // protects _sets
        static map<string,ReplicaSetMonitorPtr> _sets; // set name to Monitor

        static ConfigChangeHook _hook;
    };

    /** Use this class to connect to a replica set of servers.  The class will manage
       checking for which server in a replica set is master, and do failover automatically.

       This can also be used to connect to replica pairs since pairs are a subset of sets

       On a failover situation, expect at least one operation to return an error (throw
       an exception) before the failover is complete.  Operations are not retried.
    */
    class DBClientReplicaSet : public DBClientBase {

    public:
        /** Call connect() after constructing. autoReconnect is always on for DBClientReplicaSet connections. */
        DBClientReplicaSet( const string& name , const vector<HostAndPort>& servers );
        virtual ~DBClientReplicaSet();

        /** Returns false if nomember of the set were reachable, or neither is
         * master, although,
         * when false returned, you can still try to use this connection object, it will
         * try reconnects.
         */
        bool connect();

        /** Authorize.  Authorizes all nodes as needed
        */
        virtual bool auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword = true );

        // ----------- simple functions --------------

        /** throws userassertion "no master found" */
        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0 , int batchSize = 0 );

        /** throws userassertion "no master found" */
        virtual BSONObj findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        virtual void insert( const string &ns , BSONObj obj );

        /** insert multiple objects.  Note that single object insert is asynchronous, so this version
            is only nominally faster and not worth a special effort to try to use.  */
        virtual void insert( const string &ns, const vector< BSONObj >& v );

        virtual void remove( const string &ns , Query obj , bool justOne = 0 );

        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert = 0 , bool multi = 0 );

        virtual void killCursor( long long cursorID );

        // ---- access raw connections ----

        DBClientConnection& masterConn();
        DBClientConnection& slaveConn();

        // ---- callback pieces -------

        virtual void checkResponse( const char *data, int nReturned ) { checkMaster()->checkResponse( data , nReturned ); }

        /* this is the callback from our underlying connections to notify us that we got a "not master" error.
         */
        void isntMaster();

        // ----- status ------

        virtual bool isFailed() const { return ! _master || _master->isFailed(); }

        // ----- informational ----

        string toString() { return getServerAddress(); }

        string getServerAddress() const { return _monitor->getServerAddress(); }

        virtual ConnectionString::ConnectionType type() const { return ConnectionString::SET; }

        // ---- low level ------

        virtual bool call( Message &toSend, Message &response, bool assertOk=true , string * actualServer = 0 );
        virtual void say( Message &toSend ) { checkMaster()->say( toSend ); }
        virtual bool callRead( Message& toSend , Message& response ) { return checkMaster()->callRead( toSend , response ); }


    protected:
        virtual void sayPiggyBack( Message &toSend ) { checkMaster()->say( toSend ); }

    private:

        DBClientConnection * checkMaster();
        DBClientConnection * checkSlave();

        void _auth( DBClientConnection * conn );

        ReplicaSetMonitorPtr _monitor;

        HostAndPort _masterHost;
        scoped_ptr<DBClientConnection> _master;

        HostAndPort _slaveHost;
        scoped_ptr<DBClientConnection> _slave;

        /**
         * for storing authentication info
         * fields are exactly for DBClientConnection::auth
         */
        struct AuthInfo {
            AuthInfo( string d , string u , string p , bool di )
                : dbname( d ) , username( u ) , pwd( p ) , digestPassword( di ) {}
            string dbname;
            string username;
            string pwd;
            bool digestPassword;
        };

        // we need to store so that when we connect to a new node on failure
        // we can re-auth
        // this could be a security issue, as the password is stored in memory
        // not sure if/how we should handle
        list<AuthInfo> _auths;
    };


}
