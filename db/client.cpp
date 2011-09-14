// client.cpp

/**
*    Copyright (C) 2009 10gen Inc.
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
*/

#include "pch.h"
#include "db.h"
#include "client.h"
#include "curop-inl.h"
#include "json.h"
#include "security.h"
#include "commands.h"
#include "instance.h"
#include "../s/d_logic.h"
#include "dbwebserver.h"
#include "../util/mongoutils/html.h"
#include "../util/mongoutils/checksum.h"
#include "../util/file_allocator.h"
#include "repl/rs.h"
#include "../scripting/engine.h"

namespace mongo {

    Client* Client::syncThread;
    mongo::mutex Client::clientsMutex("clientsMutex");
    set<Client*> Client::clients; // always be in clientsMutex when manipulating this
    boost::thread_specific_ptr<Client> currentClient;

#if defined(_DEBUG)
    struct StackChecker;
    ThreadLocalValue<StackChecker *> checker;

    struct StackChecker { 
        enum { SZ = 256 * 1024 };
        char buf[SZ];
        StackChecker() { 
            checker.set(this);
        }
        void init() { 
            memset(buf, 42, sizeof(buf)); 
        }
        static void check(const char *tname) { 
            static int max;
            StackChecker *sc = checker.get();
            const char *p = sc->buf;
            int i = 0;
            for( ; i < SZ; i++ ) { 
                if( p[i] != 42 )
                    break;
            }
            int z = SZ-i;
            if( z > max ) {
                max = z;
                log() << "thread " << tname << " stack usage was " << z << " bytes" << endl;
            }
            wassert( i > 16000 );
        }
    };
#endif

    /* each thread which does db operations has a Client object in TLS.
       call this when your thread starts.
    */
    Client& Client::initThread(const char *desc, AbstractMessagingPort *mp) {
#if defined(_DEBUG)
        { 
            if( sizeof(void*) == 8 ) {
                StackChecker sc;
                sc.init();
            }
        }
#endif
        assert( currentClient.get() == 0 );
        Client *c = new Client(desc, mp);
        currentClient.reset(c);
        mongo::lastError.initThread();
        return *c;
    }

    Client::Client(const char *desc, AbstractMessagingPort *p) :
        _context(0),
        _shutdown(false),
        _desc(desc),
        _god(0),
        _lastOp(0),
        _mp(p) {
        _connectionId = setThreadName(desc);
        _curOp = new CurOp( this );
#ifndef _WIN32
        stringstream temp;
        temp << hex << showbase << pthread_self();
        _threadId = temp.str();
#endif
        scoped_lock bl(clientsMutex);
        clients.insert(this);
    }

    Client::~Client() {
        _god = 0;

        if ( _context )
            error() << "Client::~Client _context should be null but is not; client:" << _desc << endl;

        if ( ! _shutdown ) {
            error() << "Client::shutdown not called: " << _desc << endl;
        }

        if ( ! inShutdown() ) {
            // we can't clean up safely once we're in shutdown
            scoped_lock bl(clientsMutex);
            if ( ! _shutdown )
                clients.erase(this);
            delete _curOp;
        }
    }

    bool Client::shutdown() {
#if defined(_DEBUG)
        { 
            if( sizeof(void*) == 8 ) {
                StackChecker::check( desc() );
            }
        }
#endif
        _shutdown = true;
        if ( inShutdown() )
            return false;
        {
            scoped_lock bl(clientsMutex);
            clients.erase(this);
            if ( isSyncThread() ) {
                syncThread = 0;
            }
        }

        return false;
    }

    BSONObj CachedBSONObj::_tooBig = fromjson("{\"$msg\":\"query not recording (too large)\"}");
    AtomicUInt CurOp::_nextOpNum;

    Client::Context::Context( string ns , Database * db, bool doauth )
        : _client( currentClient.get() ) , _oldContext( _client->_context ) ,
          _path( dbpath ) , _lock(0) , _justCreated(false) {
        assert( db && db->isOk() );
        _ns = ns;
        _db = db;
        _client->_context = this;
        if ( doauth )
            _auth();
    }

    Client::Context::Context(const string& ns, string path , mongolock * lock , bool doauth )
        : _client( currentClient.get() ) , _oldContext( _client->_context ) ,
          _path( path ) , _lock( lock ) ,
          _ns( ns ), _db(0) {
        _finishInit( doauth );
    }

    /* this version saves the context but doesn't yet set the new one: */

    Client::Context::Context()
        : _client( currentClient.get() ) , _oldContext( _client->_context ),
          _path( dbpath ) , _lock(0) , _justCreated(false), _db(0) {
        _client->_context = this;
        clear();
    }

    void Client::Context::_finishInit( bool doauth ) {
        int lockState = dbMutex.getState();
        assert( lockState );
        
        if ( lockState > 0 && FileAllocator::get()->hasFailed() ) {
            uassert(14031, "Can't take a write lock while out of disk space", false);
        }

        _db = dbHolder.get( _ns , _path );
        if ( _db ) {
            _justCreated = false;
        }
        else if ( lockState > 0 ) {
            // already in a write lock
            _db = dbHolder.getOrCreate( _ns , _path , _justCreated );
            assert( _db );
        }
        else if ( lockState < -1 ) {
            // nested read lock :(
            assert( _lock );
            _lock->releaseAndWriteLock();
            _db = dbHolder.getOrCreate( _ns , _path , _justCreated );
            assert( _db );
        }
        else {
            // we have a read lock, but need to get a write lock for a bit
            // we need to be in a write lock since we're going to create the DB object
            // to do that, we're going to unlock, then get a write lock
            // this is so that if this is the first query and its long doesn't block db
            // we just have to check that the db wasn't closed in the interim where we unlock
            for ( int x=0; x<2; x++ ) {
                {
                    dbtemprelease unlock;
                    writelock lk( _ns );
                    dbHolder.getOrCreate( _ns , _path , _justCreated );
                }

                _db = dbHolder.get( _ns , _path );

                if ( _db )
                    break;

                log() << "db was closed on us right after we opened it: " << _ns << endl;
            }

            uassert( 13005 , "can't create db, keeps getting closed" , _db );
        }

        switch ( _client->_curOp->getOp() ) {
        case dbGetMore: // getMore's are special and should be handled else where
        case dbUpdate: // update & delete check shard version in instance.cpp, so don't check here as well
        case dbDelete:
            break;
        default: {
            string errmsg;
            if ( ! shardVersionOk( _ns , errmsg ) ) {
                ostringstream os;
                os << "[" << _ns << "] shard version not ok in Client::Context: " << errmsg;
                msgassertedNoTrace( StaleConfigInContextCode , os.str().c_str() );
            }
        }
        }

        _client->_context = this;
        _client->_curOp->enter( this );
        if ( doauth )
            _auth( lockState );
    }

    void Client::Context::_auth( int lockState ) {
        if ( _client->_ai.isAuthorizedForLock( _db->name , lockState ) )
            return;

        // before we assert, do a little cleanup
        _client->_context = _oldContext; // note: _oldContext may be null

        stringstream ss;
        ss << "unauthorized db:" << _db->name << " lock type:" << lockState << " client:" << _client->clientAddress();
        uasserted( 10057 , ss.str() );
    }

    Client::Context::~Context() {
        DEV assert( _client == currentClient.get() );
        _client->_curOp->leave( this );
        _client->_context = _oldContext; // note: _oldContext may be null
    }

    bool Client::Context::inDB( const string& db , const string& path ) const {
        if ( _path != path )
            return false;

        if ( db == _ns )
            return true;

        string::size_type idx = _ns.find( db );
        if ( idx != 0 )
            return false;

        return  _ns[db.size()] == '.';
    }

    void Client::appendLastOp( BSONObjBuilder& b ) const {
        if( theReplSet ) {
            b.append("lastOp" , (long long) _lastOp);
        }
        else {
            OpTime lo(_lastOp);
            if ( ! lo.isNull() )
                b.appendTimestamp( "lastOp" , lo.asDate() );
        }
    }


    string Client::clientAddress(bool includePort) const {
        if( _curOp )
            return _curOp->getRemoteString(includePort);
        return "";
    }

    string Client::toString() const {
        stringstream ss;
        if ( _curOp )
            ss << _curOp->infoNoauth().jsonString();
        return ss.str();
    }

    string sayClientState() {
        Client* c = currentClient.get();
        if ( !c )
            return "no client";
        return c->toString();
    }

    Client* curopWaitingForLock( int type ) {
        Client * c = currentClient.get();
        assert( c );
        CurOp * co = c->curop();
        if ( co ) {
            co->waitingForLock( type );
        }
        return c;
    }
    void curopGotLock(Client *c) {
        assert(c);
        CurOp * co = c->curop();
        if ( co )
            co->gotLock();
    }

    void KillCurrentOp::interruptJs( AtomicUInt *op ) {
        if ( !globalScriptEngine )
            return;
        if ( !op ) {
            globalScriptEngine->interruptAll();
        }
        else {
            globalScriptEngine->interrupt( *op );
        }
    }

    void KillCurrentOp::killAll() {
        _globalKill = true;
        interruptJs( 0 );
    }

    void KillCurrentOp::kill(AtomicUInt i) {
        bool found = false;
        {
            scoped_lock l( Client::clientsMutex );
            for( set< Client* >::const_iterator j = Client::clients.begin(); !found && j != Client::clients.end(); ++j ) {
                for( CurOp *k = ( *j )->curop(); !found && k; k = k->parent() ) {
                    if ( k->opNum() == i ) {
                        k->kill();
                        for( CurOp *l = ( *j )->curop(); l != k; l = l->parent() ) {
                            l->kill();
                        }
                        found = true;
                    }
                }
            }
        }
        if ( found ) {
            interruptJs( &i );
        }
    }

    CurOp::~CurOp() {
        if ( _wrapped ) {
            scoped_lock bl(Client::clientsMutex);
            _client->_curOp = _wrapped;
        }
        _client = 0;
    }

    void CurOp::enter( Client::Context * context ) {
        ensureStarted();
        setNS( context->ns() );
        _dbprofile = context->_db ? context->_db->profile : 0;
    }
    
    void CurOp::leave( Client::Context * context ) {
        unsigned long long now = curTimeMicros64();
        Top::global.record( _ns , _op , _lockType , now - _checkpoint , _command );
        _checkpoint = now;
    }


    BSONObj CurOp::infoNoauth() {
        BSONObjBuilder b;
        b.append("opid", _opNum);
        bool a = _active && _start;
        b.append("active", a);
        if ( _lockType )
            b.append("lockType" , _lockType > 0 ? "write" : "read"  );
        b.append("waitingForLock" , _waitingForLock );

        if( a ) {
            b.append("secs_running", elapsedSeconds() );
        }

        b.append( "op" , opToString( _op ) );

        b.append("ns", _ns);

        _query.append( b , "query" );

        // b.append("inLock",  ??
        stringstream clientStr;
        clientStr << _remote.toString();
        b.append("client", clientStr.str());

        if ( _client ) {
            b.append( "desc" , _client->desc() );
            if ( _client->_threadId.size() ) 
                b.append( "threadId" , _client->_threadId );
            if ( _client->_connectionId )
                b.appendNumber( "connectionId" , _client->_connectionId );
        }
        
        if ( ! _message.empty() ) {
            if ( _progressMeter.isActive() ) {
                StringBuilder buf(128);
                buf << _message.toString() << " " << _progressMeter.toString();
                b.append( "msg" , buf.str() );
                BSONObjBuilder sub( b.subobjStart( "progress" ) );
                sub.appendNumber( "done" , (long long)_progressMeter.done() );
                sub.appendNumber( "total" , (long long)_progressMeter.total() );
                sub.done();
            }
            else {
                b.append( "msg" , _message.toString() );
            }
        }

        if( killed() ) 
            b.append("killed", true);
        
        b.append( "numYields" , _numYields );

        return b.obj();
    }

    void Client::gotHandshake( const BSONObj& o ) {
        BSONObjIterator i(o);

        {
            BSONElement id = i.next();
            assert( id.type() );
            _remoteId = id.wrap( "_id" );
        }

        BSONObjBuilder b;
        while ( i.more() )
            b.append( i.next() );
        
        b.appendElementsUnique( _handshake );

        _handshake = b.obj();

        if (theReplSet && o.hasField("member")) {
            theReplSet->ghost->associateSlave(_remoteId, o["member"].Int());
        }
    }

    class HandshakeCmd : public Command {
    public:
        void help(stringstream& h) const { h << "internal"; }
        HandshakeCmd() : Command( "handshake" ) {}
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return false; }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            Client& c = cc();
            c.gotHandshake( cmdObj );
            return 1;
        }

    } handshakeCmd;

    class ClientListPlugin : public WebStatusPlugin {
    public:
        ClientListPlugin() : WebStatusPlugin( "clients" , 20 ) {}
        virtual void init() {}

        virtual void run( stringstream& ss ) {
            using namespace mongoutils::html;

            ss << "\n<table border=1 cellpadding=2 cellspacing=0>";
            ss << "<tr align='left'>"
               << th( a("", "Connections to the database, both internal and external.", "Client") )
               << th( a("http://www.mongodb.org/display/DOCS/Viewing+and+Terminating+Current+Operation", "", "OpId") )
               << "<th>Active</th>"
               << "<th>LockType</th>"
               << "<th>Waiting</th>"
               << "<th>SecsRunning</th>"
               << "<th>Op</th>"
               << th( a("http://www.mongodb.org/display/DOCS/Developer+FAQ#DeveloperFAQ-What%27sa%22namespace%22%3F", "", "Namespace") )
               << "<th>Query</th>"
               << "<th>client</th>"
               << "<th>msg</th>"
               << "<th>progress</th>"

               << "</tr>\n";
            {
                scoped_lock bl(Client::clientsMutex);
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) {
                    Client *c = *i;
                    CurOp& co = *(c->curop());
                    ss << "<tr><td>" << c->desc() << "</td>";

                    tablecell( ss , co.opNum() );
                    tablecell( ss , co.active() );
                    {
                        int lt = co.getLockType();
                        if( lt == -1 ) tablecell(ss, "R");
                        else if( lt == 1 ) tablecell(ss, "W");
                        else
                            tablecell( ss ,  lt);
                    }
                    tablecell( ss , co.isWaitingForLock() );
                    if ( co.active() )
                        tablecell( ss , co.elapsedSeconds() );
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getOp() );
                    tablecell( ss , co.getNS() );
                    if ( co.haveQuery() ) {
                        tablecell( ss , co.query() );
                    }
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getRemoteString() );

                    tablecell( ss , co.getMessage() );
                    tablecell( ss , co.getProgressMeter().toString() );


                    ss << "</tr>\n";
                }
            }
            ss << "</table>\n";

        }

    } clientListPlugin;

    int Client::recommendedYieldMicros( int * writers , int * readers ) {
        int num = 0;
        int w = 0;
        int r = 0;
        {
            scoped_lock bl(clientsMutex);
            for ( set<Client*>::iterator i=clients.begin(); i!=clients.end(); ++i ) {
                Client* c = *i;
                if ( c->curop()->isWaitingForLock() ) {
                    num++;
                    if ( c->curop()->getLockType() > 0 )
                        w++;
                    else
                        r++;
                }
            }
        }

        if ( writers )
            *writers = w;
        if ( readers )
            *readers = r;

        int time = r * 100;
        time += w * 500;

        time = min( time , 1000000 );

        // there has been a kill request for this op - we should yield to allow the op to stop
        // This function returns empty string if we aren't interrupted
        if ( killCurrentOp.checkForInterruptNoAssert( false )[0] != '\0' ) {
            return 100;
        }

        return time;
    }

    int Client::getActiveClientCount( int& writers, int& readers ) {
        writers = 0;
        readers = 0;

        scoped_lock bl(clientsMutex);
        for ( set<Client*>::iterator i=clients.begin(); i!=clients.end(); ++i ) {
            Client* c = *i;
            if ( ! c->curop()->active() )
                continue;

            int l = c->curop()->getLockType();
            if ( l > 0 )
                writers++;
            else if ( l < 0 )
                readers++;

        }

        return writers + readers;
    }

    void OpDebug::reset() {
        extra.reset();

        op = 0;
        iscommand = false;
        ns = "";
        query = BSONObj();
        updateobj = BSONObj();
        
        cursorid = 0;
        ntoreturn = 0;
        ntoskip = 0;
        exhaust = false;

        nscanned = 0;
        idhack = false;
        scanAndOrder = false;
        moved = false;
        fastmod = false;
        fastmodinsert = false;
        upsert = false;
        keyUpdates = 0;
        
        exceptionInfo.reset();
        
        executionTime = 0;
        nreturned = 0;
        responseLength = 0;
    }


#define OPDEBUG_TOSTRING_HELP(x) if( x ) s << " " #x ":" << (x)
    string OpDebug::toString() const {
        StringBuilder s( ns.size() + 64 );
        if ( iscommand )
            s << "command ";
        else
            s << opToString( op ) << ' ';
        s << ns.toString();

        if ( ! query.isEmpty() ) {
            if ( iscommand )
                s << " command: ";
            else
                s << " query: ";
            s << query.toString();
        }
        
        if ( ! updateobj.isEmpty() ) {
            s << " update: ";
            updateobj.toString( s );
        }
        
        OPDEBUG_TOSTRING_HELP( cursorid );
        OPDEBUG_TOSTRING_HELP( ntoreturn );
        OPDEBUG_TOSTRING_HELP( ntoskip );
        OPDEBUG_TOSTRING_HELP( exhaust );

        OPDEBUG_TOSTRING_HELP( nscanned );
        OPDEBUG_TOSTRING_HELP( idhack );
        OPDEBUG_TOSTRING_HELP( scanAndOrder );
        OPDEBUG_TOSTRING_HELP( moved );
        OPDEBUG_TOSTRING_HELP( fastmod );
        OPDEBUG_TOSTRING_HELP( fastmodinsert );
        OPDEBUG_TOSTRING_HELP( upsert );
        OPDEBUG_TOSTRING_HELP( keyUpdates );
        
        if ( extra.len() )
            s << " " << extra.str();

        if ( ! exceptionInfo.empty() ) {
            s << " exception: " << exceptionInfo.msg;
            if ( exceptionInfo.code )
                s << " code:" << exceptionInfo.code;
        }
        
        OPDEBUG_TOSTRING_HELP( nreturned );
        if ( responseLength )
            s << " reslen:" << responseLength;
        s << " " << executionTime << "ms";

        return s.str();
    }

#define OPDEBUG_APPEND_NUMBER(x) if( x ) b.append( #x , (x) )
#define OPDEBUG_APPEND_BOOL(x) if( x ) b.appendBool( #x , (x) )
    void OpDebug::append( const CurOp& curop, BSONObjBuilder& b ) const {
        b.append( "op" , iscommand ? "command" : opToString( op ) );
        b.append( "ns" , ns.toString() );
        if ( ! query.isEmpty() )
            b.append( iscommand ? "command" : "query" , query );
        else if ( ! iscommand && curop.haveQuery() )
            curop.appendQuery( b , "query" );

        if ( ! updateobj.isEmpty() )
            b.append( "updateobj" , updateobj );
        
        OPDEBUG_APPEND_NUMBER( cursorid );
        OPDEBUG_APPEND_NUMBER( ntoreturn );
        OPDEBUG_APPEND_NUMBER( ntoskip );
        OPDEBUG_APPEND_BOOL( exhaust );

        OPDEBUG_APPEND_NUMBER( nscanned );
        OPDEBUG_APPEND_BOOL( idhack );
        OPDEBUG_APPEND_BOOL( scanAndOrder );
        OPDEBUG_APPEND_BOOL( moved );
        OPDEBUG_APPEND_BOOL( fastmod );
        OPDEBUG_APPEND_BOOL( fastmodinsert );
        OPDEBUG_APPEND_BOOL( upsert );
        OPDEBUG_APPEND_NUMBER( keyUpdates );

        if ( ! exceptionInfo.empty() ) 
            exceptionInfo.append( b , "exception" , "exceptionCode" );
        
        OPDEBUG_APPEND_NUMBER( nreturned );
        OPDEBUG_APPEND_NUMBER( responseLength );
        b.append( "millis" , executionTime );
        
    }

}
