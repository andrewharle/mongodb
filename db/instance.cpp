// instance.cpp : Global state variables and functions.
//

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
#include "db.h"
#include "introspect.h"
#include "repl.h"
#include "dbmessage.h"
#include "instance.h"
#include "lasterror.h"
#include "security.h"
#include "json.h"
#include "replutil.h"
#include "../s/d_logic.h"
#include "../util/file_allocator.h"
#include "../util/goodies.h"
#include "cmdline.h"
#if !defined(_WIN32)
#include <sys/file.h>
#endif
#include "stats/counters.h"
#include "background.h"
#include "dur_journal.h"
#include "dur_recover.h"
#include "ops/update.h"
#include "ops/delete.h"
#include "ops/query.h"

namespace mongo {

    inline void opread(Message& m) { if( _diaglog.level & 2 ) _diaglog.readop((char *) m.singleData(), m.header()->len); }
    inline void opwrite(Message& m) { if( _diaglog.level & 1 ) _diaglog.write((char *) m.singleData(), m.header()->len); }

    void receivedKillCursors(Message& m);
    void receivedUpdate(Message& m, CurOp& op);
    void receivedDelete(Message& m, CurOp& op);
    void receivedInsert(Message& m, CurOp& op);
    bool receivedGetMore(DbResponse& dbresponse, Message& m, CurOp& curop );

    int nloggedsome = 0;
#define LOGSOME if( ++nloggedsome < 1000 || nloggedsome % 100 == 0 )

    string dbExecCommand;

    DiagLog _diaglog;

    bool useCursors = true;
    bool useHints = true;

    void flushDiagLog() {
        if( _diaglog.f && _diaglog.f->is_open() ) {
            log() << "flushing diag log" << endl;
            _diaglog.flush();
        }
    }

    KillCurrentOp killCurrentOp;

    int lockFile = 0;
#ifdef _WIN32
    HANDLE lockFileHandle;
#endif

    // see FSyncCommand:
    extern bool lockedForWriting;

    void inProgCmd( Message &m, DbResponse &dbresponse ) {
        BSONObjBuilder b;

        if( ! cc().isAdmin() ) {
            BSONObjBuilder b;
            b.append("err", "unauthorized");
        }
        else {
            DbMessage d(m);
            QueryMessage q(d);
            bool all = q.query["$all"].trueValue();
            vector<BSONObj> vals;
            {
                Client& me = cc();
                scoped_lock bl(Client::clientsMutex);
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) {
                    Client *c = *i;
                    assert( c );
                    CurOp* co = c->curop();
                    if ( c == &me && !co ) {
                        continue;
                    }
                    assert( co );
                    if( all || co->active() )
                        vals.push_back( co->infoNoauth() );
                }
            }
            b.append("inprog", vals);
            unsigned x = lockedForWriting;
            if( x ) {
                b.append("fsyncLock", x);
                b.append("info", "use db.fsyncUnlock() to terminate the fsync write/snapshot lock");
            }
        }

        replyToQuery(0, m, dbresponse, b.obj());
    }

    void killOp( Message &m, DbResponse &dbresponse ) {
        BSONObj obj;
        if( ! cc().isAdmin() ) {
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        /*else if( !dbMutexInfo.isLocked() )
            obj = fromjson("{\"info\":\"no op in progress/not locked\"}");
            */
        else {
            DbMessage d(m);
            QueryMessage q(d);
            BSONElement e = q.query.getField("op");
            if( !e.isNumber() ) {
                obj = fromjson("{\"err\":\"no op number field specified?\"}");
            }
            else {
                log() << "going to kill op: " << e << endl;
                obj = fromjson("{\"info\":\"attempting to kill op\"}");
                killCurrentOp.kill( (unsigned) e.number() );
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    void unlockFsyncAndWait();
    void unlockFsync(const char *ns, Message& m, DbResponse &dbresponse) {
        BSONObj obj;
        if ( ! cc().isAdmin() ) { // checks auth
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        else if (strncmp(ns, "admin.", 6) != 0 ) {
            obj = fromjson("{\"err\":\"unauthorized - this command must be run against the admin DB\"}");
        }
        else {
            if( lockedForWriting ) {
                log() << "command: unlock requested" << endl;
                obj = fromjson("{ok:1,\"info\":\"unlock completed\"}");
                unlockFsyncAndWait();
            }
            else {
                obj = fromjson("{ok:0,\"errmsg\":\"not locked\"}");
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    static bool receivedQuery(Client& c, DbResponse& dbresponse, Message& m ) {
        bool ok = true;
        MSGID responseTo = m.header()->id;

        DbMessage d(m);
        QueryMessage q(d);
        auto_ptr< Message > resp( new Message() );

        CurOp& op = *(c.curop());

        try {
            dbresponse.exhaust = runQuery(m, q, op, *resp);
            assert( !resp->empty() );
        }
        catch ( AssertionException& e ) {
            ok = false;
            op.debug().exceptionInfo = e.getInfo();
            LOGSOME {
                log() << "assertion " << e.toString() << " ns:" << q.ns << " query:" <<
                (q.query.valid() ? q.query.toString() : "query object is corrupt") << endl;
                if( q.ntoskip || q.ntoreturn )
                    log() << " ntoskip:" << q.ntoskip << " ntoreturn:" << q.ntoreturn << endl;
            }

            BSONObjBuilder err;
            e.getInfo().append( err );
            BSONObj errObj = err.done();

            BufBuilder b;
            b.skip(sizeof(QueryResult));
            b.appendBuf((void*) errObj.objdata(), errObj.objsize());

            // todo: call replyToQuery() from here instead of this!!! see dbmessage.h
            QueryResult * msgdata = (QueryResult *) b.buf();
            b.decouple();
            QueryResult *qr = msgdata;
            qr->_resultFlags() = ResultFlag_ErrSet;
            if ( e.getCode() == StaleConfigInContextCode )
                qr->_resultFlags() |= ResultFlag_ShardConfigStale;
            qr->len = b.len();
            qr->setOperation(opReply);
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;
            resp.reset( new Message() );
            resp->setData( msgdata, true );
        }

        op.debug().responseLength = resp->header()->dataLen();

        dbresponse.response = resp.release();
        dbresponse.responseTo = responseTo;

        return ok;
    }

    void (*reportEventToSystem)(const char *msg) = 0;

    void mongoAbort(const char *msg) { 
        if( reportEventToSystem ) 
            reportEventToSystem(msg);
        rawOut(msg);
        ::abort();
    }

    // Returns false when request includes 'end'
    void assembleResponse( Message &m, DbResponse &dbresponse, const HostAndPort& remote ) {

        // before we lock...
        int op = m.operation();
        bool isCommand = false;
        const char *ns = m.singleData()->_data + 4;
        if ( op == dbQuery ) {
            if( strstr(ns, ".$cmd") ) {
                isCommand = true;
                opwrite(m);
                if( strstr(ns, ".$cmd.sys.") ) {
                    if( strstr(ns, "$cmd.sys.inprog") ) {
                        inProgCmd(m, dbresponse);
                        return;
                    }
                    if( strstr(ns, "$cmd.sys.killop") ) {
                        killOp(m, dbresponse);
                        return;
                    }
                    if( strstr(ns, "$cmd.sys.unlock") ) {
                        unlockFsync(ns, m, dbresponse);
                        return;
                    }
                }
            }
            else {
                opread(m);
            }
        }
        else if( op == dbGetMore ) {
            opread(m);
        }
        else {
            opwrite(m);
        }

        globalOpCounters.gotOp( op , isCommand );

        Client& c = cc();

        auto_ptr<CurOp> nestedOp;
        CurOp* currentOpP = c.curop();
        if ( currentOpP->active() ) {
            nestedOp.reset( new CurOp( &c , currentOpP ) );
            currentOpP = nestedOp.get();
        }
        CurOp& currentOp = *currentOpP;
        currentOp.reset(remote,op);

        OpDebug& debug = currentOp.debug();
        debug.op = op;

        int logThreshold = cmdLine.slowMS;
        bool log = logLevel >= 1;

        if ( op == dbQuery ) {
            if ( handlePossibleShardedMessage( m , &dbresponse ) )
                return;
            receivedQuery(c , dbresponse, m );
        }
        else if ( op == dbGetMore ) {
            if ( ! receivedGetMore(dbresponse, m, currentOp) )
                log = true;
        }
        else if ( op == dbMsg ) {
            // deprecated - replaced by commands
            char *p = m.singleData()->_data;
            int len = strlen(p);
            if ( len > 400 )
                out() << curTimeMillis64() % 10000 <<
                      " long msg received, len:" << len << endl;

            Message *resp = new Message();
            if ( strcmp( "end" , p ) == 0 )
                resp->setData( opReply , "dbMsg end no longer supported" );
            else
                resp->setData( opReply , "i am fine - dbMsg deprecated");

            dbresponse.response = resp;
            dbresponse.responseTo = m.header()->id;
        }
        else {
            const char *ns = m.singleData()->_data + 4;
            char cl[256];
            nsToDatabase(ns, cl);
            if( ! c.getAuthenticationInfo()->isAuthorized(cl) ) {
                uassert_nothrow("unauthorized");
            }
            else {
                try {
                    if ( op == dbInsert ) {
                        receivedInsert(m, currentOp);
                    }
                    else if ( op == dbUpdate ) {
                        receivedUpdate(m, currentOp);
                    }
                    else if ( op == dbDelete ) {
                        receivedDelete(m, currentOp);
                    }
                    else if ( op == dbKillCursors ) {
                        currentOp.ensureStarted();
                        logThreshold = 10;
                        receivedKillCursors(m);
                    }
                    else {
                        mongo::log() << "    operation isn't supported: " << op << endl;
                        currentOp.done();
                        log = true;
                    }
                }
                catch ( UserException& ue ) {
                    tlog(3) << " Caught Assertion in " << opToString(op) << ", continuing " << ue.toString() << endl;
                    debug.exceptionInfo = ue.getInfo();
                }
                catch ( AssertionException& e ) {
                    tlog(3) << " Caught Assertion in " << opToString(op) << ", continuing " << e.toString() << endl;
                    debug.exceptionInfo = e.getInfo();
                    log = true;
                }
            }
        }
        currentOp.ensureStarted();
        currentOp.done();
        int ms = currentOp.totalTimeMillis();

        //DEV log = true;
        if ( log || ms > logThreshold ) {
            if( logLevel < 3 && op == dbGetMore && strstr(ns, ".oplog.") && ms < 4300 && !log ) {
                /* it's normal for getMore on the oplog to be slow because of use of awaitdata flag. */
            }
            else {
                debug.executionTime = ms;
                mongo::tlog() << debug << endl;
            }
        }

        if ( currentOp.shouldDBProfile( ms ) ) {
            // performance profiling is on
            if ( dbMutex.getState() < 0 ) {
                mongo::log(1) << "note: not profiling because recursive read lock" << endl;
            }
            else {
                writelock lk;
                if ( dbHolder.isLoaded( nsToDatabase( currentOp.getNS() ) , dbpath ) ) {
                    Client::Context cx( currentOp.getNS() );
                    profile(c , currentOp );
                }
                else {
                    mongo::log() << "note: not profiling because db went away - probably a close on: " << currentOp.getNS() << endl;
                }
            }
        }
        
        debug.reset();
    } /* assembleResponse() */

    void receivedKillCursors(Message& m) {
        int *x = (int *) m.singleData()->_data;
        x++; // reserved
        int n = *x++;

        uassert( 13659 , "sent 0 cursors to kill" , n != 0 );
        massert( 13658 , str::stream() << "bad kill cursors size: " << m.dataSize() , m.dataSize() == 8 + ( 8 * n ) );
        uassert( 13004 , str::stream() << "sent negative cursors to kill: " << n  , n >= 1 );

        if ( n > 2000 ) {
            log( n < 30000 ? LL_WARNING : LL_ERROR ) << "receivedKillCursors, n=" << n << endl;
            assert( n < 30000 );
        }

        int found = ClientCursor::erase(n, (long long *) x);

        if ( logLevel > 0 || found != n ) {
            log( found == n ) << "killcursors: found " << found << " of " << n << endl;
        }

    }

    /* db - database name
       path - db directory
    */
    /*static*/ void Database::closeDatabase( const char *db, const string& path ) {
        assertInWriteLock();

        Client::Context * ctx = cc().getContext();
        assert( ctx );
        assert( ctx->inDB( db , path ) );
        Database *database = ctx->db();
        assert( database->name == db );

        oplogCheckCloseDatabase( database ); // oplog caches some things, dirty its caches

        if( BackgroundOperation::inProgForDb(db) ) {
            log() << "warning: bg op in prog during close db? " << db << endl;
        }

        /* important: kill all open cursors on the database */
        string prefix(db);
        prefix += '.';
        ClientCursor::invalidate(prefix.c_str());

        NamespaceDetailsTransient::clearForPrefix( prefix.c_str() );

        dbHolder.erase( db, path );
        ctx->clear();
        delete database; // closes files
    }

    void receivedUpdate(Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        op.debug().ns = ns;
        int flags = d.pullInt();
        BSONObj query = d.nextJsObj();

        assert( d.moreJSObjs() );
        assert( query.objsize() < m.header()->dataLen() );
        BSONObj toupdate = d.nextJsObj();
        uassert( 10055 , "update object too large", toupdate.objsize() <= BSONObjMaxUserSize);
        assert( toupdate.objsize() < m.header()->dataLen() );
        assert( query.objsize() + toupdate.objsize() < m.header()->dataLen() );
        bool upsert = flags & UpdateOption_Upsert;
        bool multi = flags & UpdateOption_Multi;
        bool broadcast = flags & UpdateOption_Broadcast;
        
        op.debug().query = query;
        op.setQuery(query);

        writelock lk;

        // writelock is used to synchronize stepdowns w/ writes
        uassert( 10054 ,  "not master", isMasterNs( ns ) );
        
        // if this ever moves to outside of lock, need to adjust check Client::Context::_finishInit
        if ( ! broadcast && handlePossibleShardedMessage( m , 0 ) )
            return;

        Client::Context ctx( ns );

        UpdateResult res = updateObjects(ns, toupdate, query, upsert, multi, true, op.debug() );
        lastError.getSafe()->recordUpdate( res.existing , res.num , res.upserted ); // for getlasterror
    }

    void receivedDelete(Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        op.debug().ns = ns;
        int flags = d.pullInt();
        bool justOne = flags & RemoveOption_JustOne;
        bool broadcast = flags & RemoveOption_Broadcast;
        assert( d.moreJSObjs() );
        BSONObj pattern = d.nextJsObj();
        
        op.debug().query = pattern;
        op.setQuery(pattern);

        writelock lk(ns);

        // writelock is used to synchronize stepdowns w/ writes
        uassert( 10056 ,  "not master", isMasterNs( ns ) );

        // if this ever moves to outside of lock, need to adjust check Client::Context::_finishInit
        if ( ! broadcast && handlePossibleShardedMessage( m , 0 ) )
            return;

        Client::Context ctx(ns);

        long long n = deleteObjects(ns, pattern, justOne, true);
        lastError.getSafe()->recordDelete( n );
    }

    QueryResult* emptyMoreResult(long long);

    bool receivedGetMore(DbResponse& dbresponse, Message& m, CurOp& curop ) {
        bool ok = true;

        DbMessage d(m);

        const char *ns = d.getns();
        int ntoreturn = d.pullInt();
        long long cursorid = d.pullInt64();

        curop.debug().ns = ns;
        curop.debug().ntoreturn = ntoreturn;
        curop.debug().cursorid = cursorid;

        time_t start = 0;
        int pass = 0;
        bool exhaust = false;
        QueryResult* msgdata;
        while( 1 ) {
            try {
                readlock lk;
                Client::Context ctx(ns);
                msgdata = processGetMore(ns, ntoreturn, cursorid, curop, pass, exhaust);
            }
            catch ( AssertionException& e ) {
                exhaust = false;
                curop.debug().exceptionInfo = e.getInfo();
                msgdata = emptyMoreResult(cursorid);
                ok = false;
            }
            if (msgdata == 0) {
                exhaust = false;
                massert(13073, "shutting down", !inShutdown() );
                if( pass == 0 ) {
                    start = time(0);
                }
                else {
                    if( time(0) - start >= 4 ) {
                        // after about 4 seconds, return.  this is a sanity check.  pass stops at 1000 normally
                        // for DEV this helps and also if sleep is highly inaccurate on a platform.  we want to
                        // return occasionally so slave can checkpoint.
                        pass = 10000;
                    }
                }
                pass++;
                DEV
                sleepmillis(20);
                else
                    sleepmillis(2);
                continue;
            }
            break;
        };

        Message *resp = new Message();
        resp->setData(msgdata, true);
        curop.debug().responseLength = resp->header()->dataLen();
        curop.debug().nreturned = msgdata->nReturned;

        dbresponse.response = resp;
        dbresponse.responseTo = m.header()->id;
        
        if( exhaust ) {
            curop.debug().exhaust = true;
            dbresponse.exhaust = ns;
        }

        return ok;
    }

    void checkAndInsert(const char *ns, /*modifies*/BSONObj& js) { 
        uassert( 10059 , "object to insert too large", js.objsize() <= BSONObjMaxUserSize);
        {
            // check no $ modifiers.  note we only check top level.  (scanning deep would be quite expensive)
            BSONObjIterator i( js );
            while ( i.more() ) {
                BSONElement e = i.next();
                uassert( 13511 , "document to insert can't have $ fields" , e.fieldName()[0] != '$' );
            }
        }
        theDataFileMgr.insertWithObjMod(ns, js, false); // js may be modified in the call to add an _id field.
        logOp("i", ns, js);
    }

    NOINLINE_DECL void insertMulti(DbMessage& d, const char *ns, const BSONObj& _js) { 
        const bool keepGoing = d.reservedField() & InsertOption_ContinueOnError;
        int n = 0;
        BSONObj js(_js);
        while( 1 ) {
            try {
                checkAndInsert(ns, js);
                ++n;
                getDur().commitIfNeeded();
            } catch (const UserException&) {
                if (!keepGoing || !d.moreJSObjs()){
                    globalOpCounters.incInsertInWriteLock(n);
                    throw;
                }
                // otherwise ignore and keep going
            }
            if( !d.moreJSObjs() )
                break;
            js = d.nextJsObj(); // TODO: refactor to do objcheck outside of writelock
        }
    }

    void receivedInsert(Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        op.debug().ns = ns;

        if( !d.moreJSObjs() ) {
            // strange.  should we complain?
            return;
        }
        BSONObj js = d.nextJsObj();

        writelock lk(ns);

        // writelock is used to synchronize stepdowns w/ writes
        uassert( 10058 , "not master", isMasterNs(ns) );

        if ( handlePossibleShardedMessage( m , 0 ) )
            return;

        Client::Context ctx(ns);

        if( d.moreJSObjs() ) { 
            insertMulti(d, ns, js);
            return;
        }

        checkAndInsert(ns, js);
        globalOpCounters.incInsertInWriteLock(1);
    }

    void getDatabaseNames( vector< string > &names , const string& usePath ) {
        boost::filesystem::path path( usePath );
        for ( boost::filesystem::directory_iterator i( path );
                i != boost::filesystem::directory_iterator(); ++i ) {
            if ( directoryperdb ) {
                boost::filesystem::path p = *i;
                string dbName = p.leaf();
                p /= ( dbName + ".ns" );
                if ( MMF::exists( p ) )
                    names.push_back( dbName );
            }
            else {
                string fileName = boost::filesystem::path(*i).leaf();
                if ( fileName.length() > 3 && fileName.substr( fileName.length() - 3, 3 ) == ".ns" )
                    names.push_back( fileName.substr( 0, fileName.length() - 3 ) );
            }
        }
    }

    /* returns true if there is data on this server.  useful when starting replication.
       local database does NOT count except for rsoplog collection.
    */
    bool replHasDatabases() {
        vector<string> names;
        getDatabaseNames(names);
        if( names.size() >= 2 ) return true;
        if( names.size() == 1 ) {
            if( names[0] != "local" )
                return true;
            // we have a local database.  return true if oplog isn't empty
            {
                readlock lk(rsoplog);
                BSONObj o;
                if( Helpers::getFirst(rsoplog, o) )
                    return true;
            }
        }
        return false;
    }

    bool DBDirectClient::call( Message &toSend, Message &response, bool assertOk , string * actualServer ) {
        if ( lastError._get() )
            lastError.startRequest( toSend, lastError._get() );
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse , _clientHost );
        assert( dbResponse.response );
        dbResponse.response->concat(); // can get rid of this if we make response handling smarter
        response = *dbResponse.response;
        getDur().commitIfNeeded();
        return true;
    }

    void DBDirectClient::say( Message &toSend, bool isRetry ) {
        if ( lastError._get() )
            lastError.startRequest( toSend, lastError._get() );
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse , _clientHost );
        getDur().commitIfNeeded();
    }

    auto_ptr<DBClientCursor> DBDirectClient::query(const string &ns, Query query, int nToReturn , int nToSkip ,
            const BSONObj *fieldsToReturn , int queryOptions ) {

        //if ( ! query.obj.isEmpty() || nToReturn != 0 || nToSkip != 0 || fieldsToReturn || queryOptions )
        return DBClientBase::query( ns , query , nToReturn , nToSkip , fieldsToReturn , queryOptions );
        //
        //assert( query.obj.isEmpty() );
        //throw UserException( (string)"yay:" + ns );
    }

    void DBDirectClient::killCursor( long long id ) {
        ClientCursor::erase( id );
    }

    HostAndPort DBDirectClient::_clientHost = HostAndPort( "0.0.0.0" , 0 );

    unsigned long long DBDirectClient::count(const string &ns, const BSONObj& query, int options, int limit, int skip ) {
        readlock lk( ns );
        string errmsg;
        long long res = runCount( ns.c_str() , _countCmd( ns , query , options , limit , skip ) , errmsg );
        if ( res == -1 )
            return 0;
        uassert( 13637 , str::stream() << "count failed in DBDirectClient: " << errmsg , res >= 0 );
        return (unsigned long long )res;
    }

    DBClientBase * createDirectClient() {
        return new DBDirectClient();
    }

    mongo::mutex exitMutex("exit");
    int numExitCalls = 0;

    bool inShutdown() {
        return numExitCalls > 0;
    }

    void tryToOutputFatal( const string& s ) {
        try {
            rawOut( s );
            return;
        }
        catch ( ... ) {}

        try {
            cerr << s << endl;
            return;
        }
        catch ( ... ) {}

        // uh - oh, not sure there is anything else we can do...
    }

    /** also called by ntservice.cpp */
    void shutdownServer() {

        log() << "shutdown: going to close listening sockets..." << endl;
        ListeningSockets::get()->closeAll();

        log() << "shutdown: going to flush diaglog..." << endl;
        flushDiagLog();

        /* must do this before unmapping mem or you may get a seg fault */
        log() << "shutdown: going to close sockets..." << endl;
        boost::thread close_socket_thread( boost::bind(MessagingPort::closeAllSockets, 0) );

        // wait until file preallocation finishes
        // we would only hang here if the file_allocator code generates a
        // synchronous signal, which we don't expect
        log() << "shutdown: waiting for fs preallocator..." << endl;
        FileAllocator::get()->waitUntilFinished();

        if( cmdLine.dur ) {
            log() << "shutdown: lock for final commit..." << endl;
            {
                int n = 10;
                while( 1 ) {
                    // we may already be in a read lock from earlier in the call stack, so do read lock here 
                    // to be consistent with that.
                    readlocktry w("", 20000);
                    if( w.got() ) { 
                        log() << "shutdown: final commit..." << endl;
                        getDur().commitNow();
                        break;
                    }
                    if( --n <= 0 ) {
                        log() << "shutdown: couldn't acquire write lock, aborting" << endl;
                        mongoAbort("couldn't acquire write lock");
                    }
                    log() << "shutdown: waiting for write lock..." << endl;
                }
            }
            MemoryMappedFile::flushAll(true);
        }

        log() << "shutdown: closing all files..." << endl;
        stringstream ss3;
        MemoryMappedFile::closeAllFiles( ss3 );
        log() << ss3.str() << endl;

        if( cmdLine.dur ) {
            dur::journalCleanup(true);
        }

#if !defined(__sunos__)
        if ( lockFile ) {
            log() << "shutdown: removing fs lock..." << endl;
            /* This ought to be an unlink(), but Eliot says the last
               time that was attempted, there was a race condition
               with acquirePathLock().  */
#ifdef _WIN32
            if( _chsize( lockFile , 0 ) )
                log() << "couldn't remove fs lock " << WSAGetLastError() << endl;
            CloseHandle(lockFileHandle);
#else
            if( ftruncate( lockFile , 0 ) )
                log() << "couldn't remove fs lock " << errnoWithDescription() << endl;
            flock( lockFile, LOCK_UN );
#endif
        }
#endif
    }

    void exitCleanly( ExitCode code ) {
        killCurrentOp.killAll();
        {
            dblock lk;
            log() << "now exiting" << endl;
            dbexit( code );
        }
    }

    /* not using log() herein in case we are already locked */
    NOINLINE_DECL void dbexit( ExitCode rc, const char *why, bool tryToGetLock ) {

        auto_ptr<writelocktry> wlt;
        if ( tryToGetLock ) {
            wlt.reset( new writelocktry( "" , 2 * 60 * 1000 ) );
            uassert( 13455 , "dbexit timed out getting lock" , wlt->got() );
        }

        Client * c = currentClient.get();
        {
            scoped_lock lk( exitMutex );
            if ( numExitCalls++ > 0 ) {
                if ( numExitCalls > 5 ) {
                    // this means something horrible has happened
                    ::_exit( rc );
                }
                stringstream ss;
                ss << "dbexit: " << why << "; exiting immediately";
                tryToOutputFatal( ss.str() );
                if ( c ) c->shutdown();
                ::exit( rc );
            }
        }

        {
            stringstream ss;
            ss << "dbexit: " << why;
            tryToOutputFatal( ss.str() );
        }

        try {
            shutdownServer(); // gracefully shutdown instance
        }
        catch ( ... ) {
            tryToOutputFatal( "shutdown failed with exception" );
        }

        try {
            mutexDebugger.programEnding();
        }
        catch (...) { }

        tryToOutputFatal( "dbexit: really exiting now" );
        if ( c ) c->shutdown();
        ::exit(rc);
    }

#if !defined(__sunos__)
    void writePid(int fd) {
        stringstream ss;
        ss << getpid() << endl;
        string s = ss.str();
        const char * data = s.c_str();
#ifdef _WIN32
        assert ( _write( fd, data, strlen( data ) ) );
#else
        assert ( write( fd, data, strlen( data ) ) );
#endif
    }

    void acquirePathLock(bool doingRepair) {
        string name = ( boost::filesystem::path( dbpath ) / "mongod.lock" ).native_file_string();

        bool oldFile = false;

        if ( boost::filesystem::exists( name ) && boost::filesystem::file_size( name ) > 0 ) {
            oldFile = true;
        }

#ifdef _WIN32
        lockFileHandle = CreateFileA( name.c_str(), GENERIC_READ | GENERIC_WRITE,
            0 /* do not allow anyone else access */, NULL, 
            OPEN_ALWAYS /* success if fh can open */, 0, NULL );

        if (lockFileHandle == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            char *msg;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                (LPSTR)&msg, 0, NULL);
            string m = msg;
            str::stripTrailing(m, "\r\n");
            uasserted( 13627 , str::stream() << "Unable to create/open lock file: " << name << ' ' << m << " Is a mongod instance already running?" );
        }
        lockFile = _open_osfhandle((intptr_t)lockFileHandle, 0);
#else
        lockFile = open( name.c_str(), O_RDWR | O_CREAT , S_IRWXU | S_IRWXG | S_IRWXO );
        if( lockFile <= 0 ) {
            uasserted( 10309 , str::stream() << "Unable to create/open lock file: " << name << ' ' << errnoWithDescription() << " Is a mongod instance already running?" );
        }
        if (flock( lockFile, LOCK_EX | LOCK_NB ) != 0) {
            close ( lockFile );
            lockFile = 0;
            uassert( 10310 ,  "Unable to acquire lock for lockfilepath: " + name,  0 );
        }
#endif

        if ( oldFile ) {
            // we check this here because we want to see if we can get the lock
            // if we can't, then its probably just another mongod running
            
            string errmsg;
            if (cmdLine.dur) {
                if (!dur::haveJournalFiles()) {
                    
                    vector<string> dbnames;
                    getDatabaseNames( dbnames );
                    
                    if ( dbnames.size() == 0 ) {
                        // this means that mongod crashed
                        // between initial startup and when journaling was initialized
                        // it is safe to continue
                    }
                    else {
                        errmsg = str::stream()
                            << "************** \n"
                            << "old lock file: " << name << ".  probably means unclean shutdown,\n"
                            << "but there are no journal files to recover.\n"
                            << "this is likely human error or filesystem corruption.\n"
                            << "found " << dbnames.size() << " dbs.\n"
                            << "see: http://dochub.mongodb.org/core/repair for more information\n"
                            << "*************";
                    }


                }
            }
            else {
                if (!dur::haveJournalFiles() && !doingRepair) {
                    errmsg = str::stream()
                             << "************** \n"
                             << "Unclean shutdown detected.\n"
                             << "Please visit http://dochub.mongodb.org/core/repair for recovery instructions.\n"
                             << "*************";
                }
            }

            if (!errmsg.empty()) {
                cout << errmsg << endl;
#ifdef _WIN32
                CloseHandle( lockFileHandle );
#else
                close ( lockFile );
#endif
                lockFile = 0;
                uassert( 12596 , "old lock file" , 0 );
            }
        }

        // Not related to lock file, but this is where we handle unclean shutdown
        if( !cmdLine.dur && dur::haveJournalFiles() ) {
            cout << "**************" << endl;
            cout << "Error: journal files are present in journal directory, yet starting without --journal enabled." << endl;
            cout << "It is recommended that you start with journaling enabled so that recovery may occur." << endl;
            cout << "**************" << endl;
            uasserted(13597, "can't start without --journal enabled when journal/ files are present");
        }

#ifdef _WIN32
        uassert( 13625, "Unable to truncate lock file", _chsize(lockFile, 0) == 0);
        writePid( lockFile );
        _commit( lockFile );
#else
        uassert( 13342, "Unable to truncate lock file", ftruncate(lockFile, 0) == 0);
        writePid( lockFile );
        fsync( lockFile );
        flushMyDirectory(name);
#endif
    }
#else
    void acquirePathLock(bool) {
        // TODO - this is very bad that the code above not running here.

        // Not related to lock file, but this is where we handle unclean shutdown
        if( !cmdLine.dur && dur::haveJournalFiles() ) {
            cout << "**************" << endl;
            cout << "Error: journal files are present in journal directory, yet starting without --journal enabled." << endl;
            cout << "It is recommended that you start with journaling enabled so that recovery may occur." << endl;
            cout << "Alternatively (not recommended), you can backup everything, then delete the journal files, and run --repair" << endl;
            cout << "**************" << endl;
            uasserted(13618, "can't start without --journal enabled when journal/ files are present");
        }
    }
#endif

} // namespace mongo
