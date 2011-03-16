// client.h

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

/* Client represents a connection to the database (the server-side) and corresponds
   to an open socket (or logical connection if pooling on sockets) from a client.

   todo: switch to asio...this will fit nicely with that.
*/

#pragma once

#include "../pch.h"
#include "security.h"
#include "namespace-inl.h"
#include "lasterror.h"
#include "stats/top.h"

namespace mongo {

    extern class ReplSet *theReplSet;
    class AuthenticationInfo;
    class Database;
    class CurOp;
    class Command;
    class Client;
    class MessagingPort;

    extern boost::thread_specific_ptr<Client> currentClient;

    typedef long long ConnectionId;

    class Client : boost::noncopyable {
    public:
        class Context;

        static mongo::mutex clientsMutex;
        static set<Client*> clients; // always be in clientsMutex when manipulating this
        static int recommendedYieldMicros( int * writers = 0 , int * readers = 0 );
        static int getActiveClientCount( int& writers , int& readers );

        static Client *syncThread;


        /* each thread which does db operations has a Client object in TLS.
           call this when your thread starts.
        */
        static Client& initThread(const char *desc, MessagingPort *mp = 0);

        /*
           this has to be called as the client goes away, but before thread termination
           @return true if anything was done
         */
        bool shutdown();


        ~Client();

        void iAmSyncThread() {
            wassert( syncThread == 0 );
            syncThread = this;
        }
        bool isSyncThread() const { return this == syncThread; } // true if this client is the replication secondary pull thread


        string clientAddress(bool includePort=false) const;
        AuthenticationInfo * getAuthenticationInfo() { return &_ai; }
        bool isAdmin() { return _ai.isAuthorized( "admin" ); }
        CurOp* curop() const { return _curOp; }
        Context* getContext() const { return _context; }
        Database* database() const {  return _context ? _context->db() : 0; }
        const char *ns() const { return _context->ns(); }
        const char *desc() const { return _desc; }
        void setLastOp( ReplTime op ) { _lastOp = op; }
        ReplTime getLastOp() const { return _lastOp; }

        /* report what the last operation was.  used by getlasterror */
        void appendLastOp( BSONObjBuilder& b ) const;

        bool isGod() const { return _god; } /* this is for map/reduce writes */
        string toString() const;
        void gotHandshake( const BSONObj& o );
        BSONObj getRemoteID() const { return _remoteId; }
        BSONObj getHandshake() const { return _handshake; }

        MessagingPort * port() const { return _mp; }

        ConnectionId getConnectionId() const { return _connectionId; }

    private:
        ConnectionId _connectionId; // > 0 for things "conn", 0 otherwise
        CurOp * _curOp;
        Context * _context;
        bool _shutdown;
        const char *_desc;
        bool _god;
        AuthenticationInfo _ai;
        ReplTime _lastOp;
        BSONObj _handshake;
        BSONObj _remoteId;
        MessagingPort * const _mp;

        Client(const char *desc, MessagingPort *p = 0);

        friend class CurOp;

    public:

        /* set _god=true temporarily, safely */
        class GodScope {
            bool _prev;
        public:
            GodScope();
            ~GodScope();
        };


        /* Set database we want to use, then, restores when we finish (are out of scope)
           Note this is also helpful if an exception happens as the state if fixed up.
        */
        class Context : boost::noncopyable {
        public:
            /**
             * this is the main constructor
             * use this unless there is a good reason not to
             */
            Context(const string& ns, string path=dbpath, mongolock * lock = 0 , bool doauth=true );

            /* this version saves the context but doesn't yet set the new one: */
            Context();

            /**
             * if you are doing this after allowing a write there could be a race condition
             * if someone closes that db.  this checks that the DB is still valid
             */
            Context( string ns , Database * db, bool doauth=true );

            ~Context();

            Client* getClient() const { return _client; }
            Database* db() const { return _db; }
            const char * ns() const { return _ns.c_str(); }

            /** @return if the db was created by this Context */
            bool justCreated() const { return _justCreated; }

            bool equals( const string& ns , const string& path=dbpath ) const { return _ns == ns && _path == path; }

            /**
             * @return true iff the current Context is using db/path
             */
            bool inDB( const string& db , const string& path=dbpath ) const;

            void clear() { _ns = ""; _db = 0; }

            /**
             * call before unlocking, so clear any non-thread safe state
             */
            void unlocked() { _db = 0; }

            /**
             * call after going back into the lock, will re-establish non-thread safe stuff
             */
            void relocked() { _finishInit(); }

            friend class CurOp;

        private:
            /**
             * at this point _client, _oldContext and _ns have to be set
             * _db should not have been touched
             * this will set _db and create if needed
             * will also set _client->_context to this
             */
            void _finishInit( bool doauth=true);

            void _auth( int lockState = dbMutex.getState() );

            Client * _client;
            Context * _oldContext;

            string _path;
            mongolock * _lock;
            bool _justCreated;

            string _ns;
            Database * _db;

        }; // class Client::Context


    };

    /** get the Client object for this thread. */
    inline Client& cc() {
        Client * c = currentClient.get();
        assert( c );
        return *c;
    }

    inline Client::GodScope::GodScope() {
        _prev = cc()._god;
        cc()._god = true;
    }

    inline Client::GodScope::~GodScope() { cc()._god = _prev; }

    /* this unlocks, does NOT upgrade. that works for our current usage */
    inline void mongolock::releaseAndWriteLock() {
        if( !_writelock ) {

#if BOOST_VERSION >= 103500
            int s = dbMutex.getState();
            if( s != -1 ) {
                log() << "error: releaseAndWriteLock() s == " << s << endl;
                msgasserted( 12600, "releaseAndWriteLock: unlock_shared failed, probably recursive" );
            }
#endif

            _writelock = true;
            dbMutex.unlock_shared();
            dbMutex.lock();

            if ( cc().getContext() )
                cc().getContext()->unlocked();
        }
    }

    string sayClientState();

    inline bool haveClient() { return currentClient.get() > 0; }
};
