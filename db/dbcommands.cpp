// dbcommands.cpp

/**
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

#include "stdafx.h"
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"
#include "../util/md5.hpp"
#include "../util/processinfo.h"
#include "json.h"
#include "repl.h"
#include "replset.h"
#include "commands.h"
#include "db.h"
#include "instance.h"
#include "lasterror.h"
#include "security.h"
#include "queryoptimizer.h"
#include "../scripting/engine.h"
#include "stats/counters.h"
#include "background.h"

namespace mongo {

    TicketHolder connTicketHolder( 20000 );

    extern int otherTraceLevel;
    void flushOpLog( stringstream &ss );

    class CmdShutdown : public Command {
    public:
        virtual bool requiresAuth() { return true; }
        virtual bool adminOnly() { return true; }
        virtual bool localHostOnlyIfNoAuth(const BSONObj& cmdObj) { return true; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual LockType locktype(){ return WRITE; } 
        virtual void help( stringstream& help ) const {
            help << "shutdown the database.  must be ran against admin db and either (1) ran from localhost or (2) authenticated.\n";
        }
        CmdShutdown() : Command("shutdown") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            cc().shutdown();
            log() << "terminating, shutdown command received" << endl;
            dbexit( EXIT_CLEAN ); // this never returns
            return true;
        }
    } cmdShutdown;

    /* reset any errors so that getlasterror comes back clean.

       useful before performing a long series of operations where we want to
       see if any of the operations triggered an error, but don't want to check
       after each op as that woudl be a client/server turnaround.
    */
    class CmdResetError : public Command {
    public:
        virtual LockType locktype(){ return NONE; } 
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "reset error state (used with getpreverror)";
        }
        CmdResetError() : Command("reseterror") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            LastError *le = lastError.get();
            assert( le );
            le->reset();
            return true;
        }
    } cmdResetError;

    /* for diagnostic / testing purposes. */
    class CmdSleep : public Command { 
    public:
        virtual LockType locktype(){ return READ; } 
        virtual bool adminOnly() { return true; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "internal / make db block for 100 seconds";
        }
        CmdSleep() : Command("sleep") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            sleepsecs(100);
            return true;
        }
    } cmdSleep;

    class CmdGetLastError : public Command {
    public:
        virtual LockType locktype(){ return NONE; } 
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "return error status of the last operation";
        }
        CmdGetLastError() : Command("getlasterror") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            LastError *le = lastError.disableForCommand();
            if ( le->nPrev != 1 )
                LastError::noError.appendSelf( result );
            else
                le->appendSelf( result );
            
            if ( cmdObj["fsync"].trueValue() ){
                log() << "fsync from getlasterror" << endl;
                result.append( "fsyncFiles" , MemoryMappedFile::flushAll( true ) );
            }
            
            return true;
        }
    } cmdGetLastError;

    /* for testing purposes only */
    class CmdForceError : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual LockType locktype(){ return NONE; } 
        CmdForceError() : Command("forceerror") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            uassert( 10038 , "forced error", false);
            return true;
        }
    } cmdForceError;

    class CmdGetPrevError : public Command {
    public:
        virtual LockType locktype(){ return NONE; } 
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual void help( stringstream& help ) const {
            help << "check for errors since last reseterror commandcal";
        }
        virtual bool slaveOk() {
            return true;
        }
        CmdGetPrevError() : Command("getpreverror") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            LastError *le = lastError.disableForCommand();
            le->appendSelf( result );
            if ( le->valid )
                result.append( "nPrev", le->nPrev );
            else
                result.append( "nPrev", -1 );
            return true;
        }
    } cmdGetPrevError;

    class CmdSwitchToClientErrors : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual void help( stringstream& help ) const {
            help << "convert to id based errors rather than connection based";
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual LockType locktype(){ return NONE; } 
        CmdSwitchToClientErrors() : Command("switchtoclienterrors") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if ( lastError.getID() ){
                errmsg = "already in client id mode";
                return false;
            }
            LastError *le = lastError.disableForCommand();
            le->overridenById = true;
            result << "ok" << 1;
            return true;
        }
    } cmdSwitchToClientErrors;

    class CmdDropDatabase : public Command {
    public:
        virtual bool logTheOp() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "drop (delete) this database";
        }
        virtual bool slaveOk() {
            return false;
        }
        virtual LockType locktype(){ return WRITE; } 
        CmdDropDatabase() : Command("dropDatabase") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.getField(name);
            log() << "dropDatabase " << ns << endl;
            int p = (int) e.number();
            if ( p != 1 )
                return false;
            dropDatabase(ns);
            result.append( "dropped" , ns );
            return true;
        }
    } cmdDropDatabase;

    class CmdRepairDatabase : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "repair database.  also compacts. note: slow.";
        }
        virtual LockType locktype(){ return WRITE; } 
        CmdRepairDatabase() : Command("repairDatabase") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.getField(name);
            log() << "repairDatabase " << ns << endl;
            int p = (int) e.number();
            if ( p != 1 )
                return false;
            e = cmdObj.getField( "preserveClonedFilesOnFailure" );
            bool preserveClonedFilesOnFailure = e.isBoolean() && e.boolean();
            e = cmdObj.getField( "backupOriginalFiles" );
            bool backupOriginalFiles = e.isBoolean() && e.boolean();
            return repairDatabase( ns, errmsg, preserveClonedFilesOnFailure, backupOriginalFiles );
        }
    } cmdRepairDatabase;
    
    /* set db profiling level
       todo: how do we handle profiling information put in the db with replication?
             sensibly or not?
    */
    class CmdProfile : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "enable or disable performance profiling";
        }
        virtual LockType locktype(){ return WRITE; } 
        CmdProfile() : Command("profile") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONElement e = cmdObj.getField(name);
            result.append("was", (double) cc().database()->profile);
            int p = (int) e.number();
            bool ok = false;
            if ( p == -1 )
                ok = true;
            else if ( p >= 0 && p <= 2 ) {
                ok = cc().database()->setProfilingLevel( p , errmsg );
            }

            BSONElement slow = cmdObj["slowms"];
            if ( slow.isNumber() )
                cmdLine.slowMS = slow.numberInt();
            
            return ok;
        }
    } cmdProfile;

    class CmdServerStatus : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        CmdServerStatus() : Command("serverStatus") {
            started = time(0);
        }
        
        virtual LockType locktype(){ return NONE; } 

        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            
			bool authed = cc().getAuthenticationInfo()->isAuthorizedReads("admin");

            result.append("uptime",(double) (time(0)-started));
            result.appendDate( "localTime" , jsTime() );
            
            {
                BSONObjBuilder t;

                unsigned long long last, start, timeLocked;
                dbMutex.info().getTimingInfo(start, timeLocked);
                last = curTimeMicros64();
                double tt = (double) last-start;
                double tl = (double) timeLocked;
                t.append("totalTime", tt);
                t.append("lockTime", tl);
                t.append("ratio", (tt ? tl/tt : 0));
                
                result.append( "globalLock" , t.obj() );
            }
            
            if ( authed ){
                
                BSONObjBuilder t( result.subobjStart( "mem" ) );
                
                ProcessInfo p;
                if ( p.supported() ){
                    t.appendNumber( "resident" , p.getResidentSize() );
                    t.appendNumber( "virtual" , p.getVirtualMemorySize() );
                    t.appendBool( "supported" , true );
                }
                else {
                    result.append( "note" , "not all mem info support on this platform" );
                    t.appendBool( "supported" , false );
                }
                    
                t.appendNumber( "mapped" , MemoryMappedFile::totalMappedLength() / ( 1024 * 1024 ) );

                t.done();
                    
            }
            
            {
                BSONObjBuilder bb( result.subobjStart( "connections" ) );
                bb.append( "current" , connTicketHolder.used() );
                bb.append( "available" , connTicketHolder.available() );
                bb.done();
            }
            
            if ( authed ){
                BSONObjBuilder bb( result.subobjStart( "extra_info" ) );
                bb.append("note", "fields vary by platform");
                ProcessInfo p;
                p.getExtraInfo(bb);
                bb.done();
            }


            {
                BSONObjBuilder bb( result.subobjStart( "indexCounters" ) );
                globalIndexCounters.append( bb );
                bb.done();
            }

            {
                BSONObjBuilder bb( result.subobjStart( "backgroundFlushing" ) );
                globalFlushCounters.append( bb );
                bb.done();
            }
            
            if ( anyReplEnabled() ){
                BSONObjBuilder bb( result.subobjStart( "repl" ) );
                appendReplicationInfo( bb , authed , cmdObj["repl"].numberInt() );
                bb.done();
            }
            
            result.append( "opcounters" , globalOpCounters.getObj() );
            
            {
                BSONObjBuilder asserts( result.subobjStart( "asserts" ) );
                asserts.append( "regular" , assertionCount.regular );
                asserts.append( "warning" , assertionCount.warning );
                asserts.append( "msg" , assertionCount.msg );
                asserts.append( "user" , assertionCount.user );
                asserts.append( "rollovers" , assertionCount.rollovers );
                asserts.done();
            }

            if ( ! authed )
                result.append( "note" , "run against admin for more info" );

            return true;
        }
        time_t started;
    } cmdServerStatus;

    /* just to check if the db has asserted */
    class CmdAssertInfo : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << "check if any asserts have occurred on the server";
        }
        virtual LockType locktype(){ return WRITE; } 
        CmdAssertInfo() : Command("assertinfo") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            result.appendBool("dbasserted", lastAssert[0].isSet() || lastAssert[1].isSet() || lastAssert[2].isSet());
            result.appendBool("asserted", lastAssert[0].isSet() || lastAssert[1].isSet() || lastAssert[2].isSet() || lastAssert[3].isSet());
            result.append("assert", lastAssert[AssertRegular].toString());
            result.append("assertw", lastAssert[AssertW].toString());
            result.append("assertmsg", lastAssert[AssertMsg].toString());
            result.append("assertuser", lastAssert[AssertUser].toString());
            return true;
        }
    } cmdAsserts;

    class CmdGetOpTime : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        virtual LockType locktype(){ return NONE; } 
        CmdGetOpTime() : Command("getoptime") { }
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            writelock l( "" );
            result.appendDate("optime", OpTime::now().asDate());
            return true;
        }
    } cmdgetoptime;

    /*
    class Cmd : public Command {
    public:
        Cmd() : Command("") { }
        bool adminOnly() { return true; }
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result) {
            return true;
        }
    } cmd;
    */

    class CmdDiagLogging : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        CmdDiagLogging() : Command("diagLogging") { }
        bool adminOnly() {
            return true;
        }
        virtual LockType locktype(){ return WRITE; } 
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            int was = _diaglog.setLevel( cmdObj.firstElement().numberInt() );
            stringstream ss;
            flushOpLog( ss );
            out() << ss.str() << endl;
            if ( !cmdLine.quiet )
                log() << "CMD: diagLogging set to " << _diaglog.level << " from: " << was << endl;
            result.append( "was" , was );
            return true;
        }
    } cmddiaglogging;

    /* remove bit from a bit array - actually remove its slot, not a clear
       note: this function does not work with x == 63 -- that is ok
             but keep in mind in the future if max indexes were extended to 
             exactly 64 it would be a problem
    */
    unsigned long long removeBit(unsigned long long b, int x) {
        unsigned long long tmp = b;
        return
            (tmp & ((((unsigned long long) 1) << x)-1)) |
            ((tmp >> (x+1)) << x);
    }

    struct DBCommandsUnitTest {
        DBCommandsUnitTest() {
            assert( removeBit(1, 0) == 0 );
            assert( removeBit(2, 0) == 1 );
            assert( removeBit(2, 1) == 0 );
            assert( removeBit(255, 1) == 127 );
            assert( removeBit(21, 2) == 9 );
            assert( removeBit(0x4000000000000001ULL, 62) == 1 );
        }
    } dbc_unittest;

    void assureSysIndexesEmptied(const char *ns, IndexDetails *exceptForIdIndex);
    int removeFromSysIndexes(const char *ns, const char *idxName);

    bool dropIndexes( NamespaceDetails *d, const char *ns, const char *name, string &errmsg, BSONObjBuilder &anObjBuilder, bool mayDeleteIdIndex ) {

        BackgroundOperation::assertNoBgOpInProgForNs(ns);

        d->aboutToDeleteAnIndex();

        /* there may be pointers pointing at keys in the btree(s).  kill them. */
        ClientCursor::invalidate(ns);

        // delete a specific index or all?
        if ( *name == '*' && name[1] == 0 ) {
            log(4) << "  d->nIndexes was " << d->nIndexes << '\n';
            anObjBuilder.append("nIndexesWas", (double)d->nIndexes);
            IndexDetails *idIndex = 0;
            if( d->nIndexes ) {
                for ( int i = 0; i < d->nIndexes; i++ ) {
                    if ( !mayDeleteIdIndex && d->idx(i).isIdIndex() ) {
                        idIndex = &d->idx(i);
                    } else {
                        d->idx(i).kill_idx();
                    }
                }
                d->nIndexes = 0;
            }
            if ( idIndex ) {
                d->addIndex(ns) = *idIndex;
                wassert( d->nIndexes == 1 );
            }
            /* assuming here that id index is not multikey: */
            d->multiKeyIndexBits = 0;
            assureSysIndexesEmptied(ns, idIndex);
            anObjBuilder.append("msg", mayDeleteIdIndex ? 
                "indexes dropped for collection" : 
                "non-_id indexes dropped for collection");
        }
        else {
            // delete just one index
            int x = d->findIndexByName(name);
            if ( x >= 0 ) {
                log(4) << "  d->nIndexes was " << d->nIndexes << endl;
                anObjBuilder.append("nIndexesWas", (double)d->nIndexes);

                /* note it is  important we remove the IndexDetails with this
                 call, otherwise, on recreate, the old one would be reused, and its
                 IndexDetails::info ptr would be bad info.
                 */
                IndexDetails *id = &d->idx(x);
                if ( !mayDeleteIdIndex && id->isIdIndex() ) {
                    errmsg = "may not delete _id index";
                    return false;
                }
                id->kill_idx();
                d->multiKeyIndexBits = removeBit(d->multiKeyIndexBits, x);
                d->nIndexes--;
                for ( int i = x; i < d->nIndexes; i++ )
                    d->idx(i) = d->idx(i+1);
            } else {
                int n = removeFromSysIndexes(ns, name); // just in case an orphaned listing there - i.e. should have been repaired but wasn't
                if( n ) { 
                    log() << "info: removeFromSysIndexes cleaned up " << n << " entries" << endl;
                }
                log() << "dropIndexes: " << name << " not found" << endl;
                errmsg = "index not found";
                return false;
            }
        }
        return true;
    }

    /* drop collection */
    class CmdDrop : public Command {
    public:
        CmdDrop() : Command("drop") { }
        virtual bool logTheOp() {
            return true;
        }
        virtual bool slaveOk() {
            return false;
        }
        virtual bool adminOnly() {
            return false;
        }
        virtual LockType locktype(){ return WRITE; } 
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            string nsToDrop = cc().database()->name + '.' + cmdObj.getField(name).valuestr();
            NamespaceDetails *d = nsdetails(nsToDrop.c_str());
            if ( !cmdLine.quiet )
                log() << "CMD: drop " << nsToDrop << endl;
            if ( d == 0 ) {
                errmsg = "ns not found";
                return false;
            }
            uassert( 10039 ,  "can't drop collection with reserved $ character in name", strchr(nsToDrop.c_str(), '$') == 0 );
            dropCollection( nsToDrop, errmsg, result );
            return true;
        }
    } cmdDrop;

    /* select count(*) */
    class CmdCount : public Command {
    public:
        virtual LockType locktype(){ return READ; } 
        CmdCount() : Command("count") { }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            // ok on --slave setups, not ok for nonmaster of a repl pair (unless override)
            return replSettings.slave == SimpleSlave;
        }
        virtual bool slaveOverrideOk() {
            return true;
        }
        virtual bool adminOnly() {
            return false;
        }
        virtual bool run(const char *_ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            string ns = cc().database()->name + '.' + cmdObj.getField(name).valuestr();
            string err;
            long long n = runCount(ns.c_str(), cmdObj, err);
            long long nn = n;
            bool ok = true;
            if ( n == -1 ){
                nn = 0;
                result.appendBool( "missing" , true );
            }
            else if ( n < 0 ) {
                nn = 0;
                ok = false;
                if ( !err.empty() )
                    errmsg = err;
            }
            result.append("n", (double) nn);
            return ok;
        }
    } cmdCount;

    /* create collection */
    class CmdCreate : public Command {
    public:
        CmdCreate() : Command("create") { }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return false;
        }
        virtual bool adminOnly() {
            return false;
        }
        virtual LockType locktype(){ return WRITE; } 
        virtual void help( stringstream& help ) const {
            help << "create a collection";
        }
        virtual bool run(const char *_ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            string ns = cc().database()->name + '.' + cmdObj.getField(name).valuestr();
            string err;
            bool ok = userCreateNS(ns.c_str(), cmdObj, err, true);
            if ( !ok && !err.empty() )
                errmsg = err;
            return ok;
        }
    } cmdCreate;

    /* "dropIndexes" is now the preferred form - "deleteIndexes" deprecated */
    class CmdDropIndexes : public Command {
    public:
        virtual bool logTheOp() {
            return true;
        }
        virtual bool slaveOk() {
            return false;
        }
        virtual LockType locktype(){ return WRITE; } 
        virtual void help( stringstream& help ) const {
            help << "drop indexes for a collection";
        }
        CmdDropIndexes(const char *cmdname = "dropIndexes") : Command(cmdname) { }
        bool run(const char *ns, BSONObj& jsobj, string& errmsg, BSONObjBuilder& anObjBuilder, bool /*fromRepl*/) {
            BSONElement e = jsobj.getField(name.c_str());
            string toDeleteNs = cc().database()->name + '.' + e.valuestr();
            NamespaceDetails *d = nsdetails(toDeleteNs.c_str());
            if ( !cmdLine.quiet )
                log() << "CMD: dropIndexes " << toDeleteNs << endl;
            if ( d ) {
                BSONElement f = jsobj.getField("index");
                if ( f.type() == String ) {
                    return dropIndexes( d, toDeleteNs.c_str(), f.valuestr(), errmsg, anObjBuilder, false );
                }
                else if ( f.type() == Object ){
                    int idxId = d->findIndexByKeyPattern( f.embeddedObject() );
                    if ( idxId < 0 ){
                        errmsg = "can't find index with key:";
                        errmsg += f.embeddedObject();
                        return false;
                    }
                    else {
                        IndexDetails& ii = d->idx( idxId );
                        string iName = ii.indexName();
                        return dropIndexes( d, toDeleteNs.c_str(), iName.c_str() , errmsg, anObjBuilder, false );
                    }
                }
                else {
                    errmsg = "invalid index name spec";
                    return false;
                }
            }
            else {
                errmsg = "ns not found";
                return false;
            }
        }
    } cmdDropIndexes;
    class CmdDeleteIndexes : public CmdDropIndexes { 
    public:
        CmdDeleteIndexes() : CmdDropIndexes("deleteIndexes") { }
    } cmdDeleteIndexes;

    class CmdReIndex : public Command {
    public:
        virtual bool logTheOp() {
            return true;
        }
        virtual bool slaveOk() {
            return false;
        }
        virtual LockType locktype(){ return WRITE; } 
        virtual void help( stringstream& help ) const {
            help << "re-index a collection";
        }
        CmdReIndex() : Command("reIndex") { }
        bool run(const char *ns, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            BackgroundOperation::assertNoBgOpInProgForNs(ns);

            static DBDirectClient db;

            BSONElement e = jsobj.getField(name.c_str());
            string toDeleteNs = cc().database()->name + '.' + e.valuestr();
            NamespaceDetails *d = nsdetails(toDeleteNs.c_str());
            log() << "CMD: reIndex " << toDeleteNs << endl;

            if ( ! d ){
                errmsg = "ns not found";
                return false;
            }

            list<BSONObj> all;
            auto_ptr<DBClientCursor> i = db.getIndexes( toDeleteNs );
            BSONObjBuilder b;
            while ( i->more() ){
                BSONObj o = i->next().getOwned();
                b.append( BSONObjBuilder::numStr( all.size() ) , o );
                all.push_back( o );
            }


            bool ok = dropIndexes( d, toDeleteNs.c_str(), "*" , errmsg, result, true );
            if ( ! ok ){
                errmsg = "dropIndexes failed";
                return false;
            }

            for ( list<BSONObj>::iterator i=all.begin(); i!=all.end(); i++ ){
                BSONObj o = *i;
                db.insert( Namespace( toDeleteNs.c_str() ).getSisterNS( "system.indexes" ).c_str() , o );
            }

            result.append( "ok" , 1 );
            result.append( "nIndexes" , (int)all.size() );
            result.appendArray( "indexes" , b.obj() );
            return true;
        }
    } cmdReIndex;

    class CmdListDatabases : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual bool slaveOverrideOk() {
            return true;
        }
        virtual bool adminOnly() {
            return true;
        }
        virtual LockType locktype(){ return WRITE; } 
        CmdListDatabases() : Command("listDatabases") {}
        bool run(const char *ns, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            vector< string > dbNames;
            getDatabaseNames( dbNames );
            vector< BSONObj > dbInfos;

            set<string> seen;
            boost::intmax_t totalSize = 0;
            for ( vector< string >::iterator i = dbNames.begin(); i != dbNames.end(); ++i ) {
                BSONObjBuilder b;
                b.append( "name", i->c_str() );
                boost::intmax_t size = dbSize( i->c_str() );
                b.append( "sizeOnDisk", (double) size );
                Client::Context ctx( *i );
                b.appendBool( "empty", ctx.db()->isEmpty() );
                totalSize += size;
                dbInfos.push_back( b.obj() );

                seen.insert( i->c_str() );
            }
            
            // TODO: erh 1/1/2010 I think this is broken where path != dbpath ??
            set<string> allShortNames;
            dbHolder.getAllShortNames( allShortNames );
            for ( set<string>::iterator i = allShortNames.begin(); i != allShortNames.end(); i++ ){
                string name = *i;

                if ( seen.count( name ) )
                    continue;

                BSONObjBuilder b;
                b << "name" << name << "sizeOnDisk" << double( 1 );
                Client::Context ctx( name );
                b.appendBool( "empty", ctx.db()->isEmpty() );

                dbInfos.push_back( b.obj() );
            }

            result.append( "databases", dbInfos );
            result.append( "totalSize", double( totalSize ) );
            return true;
        }
    } cmdListDatabases;

    /* note an access to a database right after this will open it back up - so this is mainly 
       for diagnostic purposes. 
       */
    class CmdCloseAllDatabases : public Command {
    public:
        virtual bool adminOnly() { return true; }
        virtual bool slaveOk() { return false; }
        virtual LockType locktype(){ return WRITE; } 
        CmdCloseAllDatabases() : Command( "closeAllDatabases" ) {}
        bool run(const char *ns, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            return dbHolder.closeAll( dbpath , result, false );
        }
    } cmdCloseAllDatabases;

    class CmdFileMD5 : public Command {
    public:
        CmdFileMD5() : Command( "filemd5" ){}
        virtual bool slaveOk() {
            return true;
        }
        virtual void help( stringstream& help ) const {
            help << " example: { filemd5 : ObjectId(aaaaaaa) , key : { ts : 1 } }";
        }
        virtual LockType locktype(){ return READ; } 
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            static DBDirectClient db;

            string ns = nsToDatabase( dbname );
            ns += ".";
            {
                string root = jsobj.getStringField( "root" );
                if ( root.size() == 0 )
                    root = "fs";
                ns += root;
            }
            ns += ".chunks"; // make this an option in jsobj

            BSONObjBuilder query;
            query.appendAs( jsobj["filemd5"] , "files_id" );
            Query q( query.obj() );
            q.sort( BSON( "files_id" << 1 << "n" << 1 ) );

            md5digest d;
            md5_state_t st;
            md5_init(&st);

            dbtemprelease temp;

            auto_ptr<DBClientCursor> cursor = db.query( ns.c_str() , q );
            int n = 0;
            while ( cursor->more() ){
                BSONObj c = cursor->next();
                int myn = c.getIntField( "n" );
                if ( n != myn ){
                    log() << "should have chunk: " << n << " have:" << myn << endl;
                    uassert( 10040 ,  "chunks out of order" , n == myn );
                }

                int len;
                const char * data = c["data"].binData( len );
                md5_append( &st , (const md5_byte_t*)(data + 4) , len - 4 );

                n++;
            }
            md5_finish(&st, d);

            result.append( "md5" , digestToString( d ) );
            return true;
        }
    } cmdFileMD5;

    IndexDetails *cmdIndexDetailsForRange( const char *ns, string &errmsg, BSONObj &min, BSONObj &max, BSONObj &keyPattern ) {
        if ( ns[ 0 ] == '\0' || min.isEmpty() || max.isEmpty() ) {
            errmsg = "invalid command syntax (note: min and max are required)";
            return 0;
        }
        return indexDetailsForRange( ns, errmsg, min, max, keyPattern );
    }

    class CmdMedianKey : public Command {
    public:
        CmdMedianKey() : Command( "medianKey" ) {}
        virtual bool slaveOk() { return true; }
        virtual LockType locktype(){ return READ; } 
        virtual void help( stringstream &help ) const {
            help << " example: { medianKey:\"blog.posts\", keyPattern:{x:1}, min:{x:10}, max:{x:55} }\n"
                "NOTE: This command may take awhile to run";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            const char *ns = jsobj.getStringField( "medianKey" );
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );
            
            Client::Context ctx( ns );

            IndexDetails *id = cmdIndexDetailsForRange( ns, errmsg, min, max, keyPattern );
            if ( id == 0 )
                return false;

            Timer t;
            int num = 0;
            NamespaceDetails *d = nsdetails(ns);
            int idxNo = d->idxNo(*id);
            for( BtreeCursor c( d, idxNo, *id, min, max, false, 1 ); c.ok(); c.advance(), ++num );
            num /= 2;
            BtreeCursor c( d, idxNo, *id, min, max, false, 1 );
            for( ; num; c.advance(), --num );
            int ms = t.millis();
            if ( ms > cmdLine.slowMS ) {
                out() << "Finding median for index: " << keyPattern << " between " << min << " and " << max << " took " << ms << "ms." << endl;
            }

            if ( !c.ok() ) {
                errmsg = "no index entries in the specified range";
                return false;
            }

            result.append( "median", c.prettyKey( c.currKey() ) );
            return true;
        }
    } cmdMedianKey;

    class CmdDatasize : public Command {
    public:
        CmdDatasize() : Command( "datasize" ) {}
        virtual bool slaveOk() { return true; }
        virtual LockType locktype(){ return READ; } 
        virtual void help( stringstream &help ) const {
            help <<
                "\ndetermine data size for a set of data in a certain range"
                "\nexample: { datasize:\"blog.posts\", keyPattern:{x:1}, min:{x:10}, max:{x:55} }"
                "\nkeyPattern, min, and max parameters are optional."
                "\nnot: This command may take a while to run";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            const char *ns = jsobj.getStringField( "datasize" );
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );

            Client::Context ctx( ns );
            
            auto_ptr< Cursor > c;
            if ( min.isEmpty() && max.isEmpty() ) {
                c = theDataFileMgr.findAll( ns );
            } else if ( min.isEmpty() || max.isEmpty() ) {
                errmsg = "only one of min or max specified";
                return false;
            } else {
                IndexDetails *idx = cmdIndexDetailsForRange( ns, errmsg, min, max, keyPattern );
                if ( idx == 0 )
                    return false;
                NamespaceDetails *d = nsdetails(ns);
                c.reset( new BtreeCursor( d, d->idxNo(*idx), *idx, min, max, false, 1 ) );
            }

            Timer t;
            long long size = 0;
            long long numObjects = 0;
            while( c->ok() ) {
                size += c->current().objsize();
                c->advance();
                numObjects++;
            }
            int ms = t.millis();
            if ( ms > cmdLine.slowMS ) {
                if ( min.isEmpty() ) {
                    out() << "Finding size for ns: " << ns << " took " << ms << "ms." << endl;
                } else {
                    out() << "Finding size for ns: " << ns << " between " << min << " and " << max << " took " << ms << "ms." << endl;
                }
            }

            result.append( "size", (double)size );
            result.append( "numObjects" , (double)numObjects );
            return true;
        }
    } cmdDatasize;

    namespace {
        long long getIndexSizeForCollection(string db, string ns, BSONObjBuilder* details=NULL, int scale = 1 ){
            DBDirectClient client;
            auto_ptr<DBClientCursor> indexes =
                client.query(db + ".system.indexes", QUERY( "ns" << ns));

            long long totalSize = 0;
            while (indexes->more()){
                BSONObj index = indexes->nextSafe();
                NamespaceDetails * nsd = nsdetails( (ns + ".$" + index["name"].valuestrsafe()).c_str() );
                if (!nsd)
                    continue; // nothing to do here
                totalSize += nsd->datasize;
                if (details)
                    details->appendNumber(index["name"].valuestrsafe(), nsd->datasize / scale );
            }
            return totalSize;
        }
    }

    class CollectionStats : public Command {
    public:
        CollectionStats() : Command( "collstats" ) {}
        virtual bool slaveOk() { return true; }
        virtual LockType locktype(){ return READ; } 
        virtual void help( stringstream &help ) const {
            help << " example: { collstats:\"blog.posts\" } ";
        }
        bool run(const char *dbname_c, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            string dbname = dbname_c;
            if ( dbname.find( "." ) != string::npos )
                dbname = dbname.substr( 0 , dbname.find( "." ) );
            
            string ns = dbname + "." + jsobj.firstElement().valuestr();

            NamespaceDetails * nsd = nsdetails( ns.c_str() );
            if ( ! nsd ){
                errmsg = "ns not found";
                return false;
            }

            result.append( "ns" , ns.c_str() );
            
            int scale = 1;
            if ( jsobj["scale"].isNumber() )
                scale = jsobj["scale"].numberInt();

            result.appendNumber( "count" , nsd->nrecords );
            result.appendNumber( "size" , nsd->datasize / scale );
            int numExtents;
            result.appendNumber( "storageSize" , nsd->storageSize( &numExtents ) / scale );
            result.append( "numExtents" , numExtents );
            result.append( "nindexes" , nsd->nIndexes );
            result.append( "lastExtentSize" , nsd->lastExtentSize / scale );
            result.append( "paddingFactor" , nsd->paddingFactor );
            result.append( "flags" , nsd->flags );

            BSONObjBuilder indexSizes;
            result.appendNumber( "totalIndexSize" , getIndexSizeForCollection(dbname, ns, &indexSizes, scale) / scale );
            result.append("indexSizes", indexSizes.obj());
            
            if ( nsd->capped ){
                result.append( "capped" , nsd->capped );
                result.append( "max" , nsd->max );
            }

            return true;
        }
    } cmdCollectionStatis;


    class DBStats : public Command {
    public:
        DBStats() : Command( "dbstats" ) {}
        virtual bool slaveOk() { return true; }
        virtual LockType locktype(){ return READ; } 
        virtual void help( stringstream &help ) const {
            help << " example: { dbstats:1 } ";
        }
        bool run(const char *dbname_c, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            string dbname = dbname_c;
            if ( dbname.find( "." ) != string::npos )
                dbname = dbname.substr( 0 , dbname.find( "." ) );

            DBDirectClient client;
            const list<string> collections = client.getCollectionNames(dbname);

            long long ncollections = 0;
            long long objects = 0;
            long long size = 0;
            long long storageSize = 0;
            long long numExtents = 0;
            long long indexes = 0;
            long long indexSize = 0;

            for (list<string>::const_iterator it = collections.begin(); it != collections.end(); ++it){
                const string ns = *it;

                NamespaceDetails * nsd = nsdetails( ns.c_str() );
                if ( ! nsd ){
                    // should this assert here?
                    continue;
                }

                ncollections += 1;
                objects += nsd->nrecords;
                size += nsd->datasize;

                int temp;
                storageSize += nsd->storageSize( &temp );
                numExtents += temp;

                indexes += nsd->nIndexes;
                indexSize += getIndexSizeForCollection(dbname, ns);
            }

            result.appendNumber( "collections" , ncollections );
            result.appendNumber( "objects" , objects );
            result.appendNumber( "dataSize" , size );
            result.appendNumber( "storageSize" , storageSize);
            result.appendNumber( "numExtents" , numExtents );
            result.appendNumber( "indexes" , indexes );
            result.appendNumber( "indexSize" , indexSize );

                return true;
        }
    } cmdDBStats;

    class CmdBuildInfo : public Command {
    public:
        CmdBuildInfo() : Command( "buildinfo" ) {}
        virtual bool slaveOk() { return true; }
        virtual bool adminOnly() { return true; }
        virtual LockType locktype(){ return NONE; } 
        virtual void help( stringstream &help ) const {
            help << "example: { buildinfo:1 }";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            result << "version" << versionString << "gitVersion" << gitVersion() << "sysInfo" << sysInfo();
            result << "bits" << ( sizeof( int* ) == 4 ? 32 : 64 );
            return true;
        }
    } cmdBuildInfo;

    /* convertToCapped seems to use this */
    class CmdCloneCollectionAsCapped : public Command {
    public:
        CmdCloneCollectionAsCapped() : Command( "cloneCollectionAsCapped" ) {}
        virtual bool slaveOk() { return false; }
        virtual LockType locktype(){ return WRITE; } 
        virtual void help( stringstream &help ) const {
            help << "example: { cloneCollectionAsCapped:<fromName>, toCollection:<toName>, size:<sizeInBytes> }";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            string from = jsobj.getStringField( "cloneCollectionAsCapped" );
            string to = jsobj.getStringField( "toCollection" );
            long long size = (long long)jsobj.getField( "size" ).number();

            if ( from.empty() || to.empty() || size == 0 ) {
                errmsg = "invalid command spec";
                return false;
            }

            char realDbName[256];
            nsToDatabase( dbname, realDbName );

            string fromNs = string( realDbName ) + "." + from;
            string toNs = string( realDbName ) + "." + to;
            NamespaceDetails *nsd = nsdetails( fromNs.c_str() );
            massert( 10301 ,  "source collection " + fromNs + " does not exist", nsd );
            long long excessSize = nsd->datasize - size * 2; // datasize and extentSize can't be compared exactly, so add some padding to 'size'
            DiskLoc extent = nsd->firstExtent;
            for( ; excessSize > extent.ext()->length && extent != nsd->lastExtent; extent = extent.ext()->xnext ) {
                excessSize -= extent.ext()->length;
                log( 2 ) << "cloneCollectionAsCapped skipping extent of size " << extent.ext()->length << endl;
                log( 6 ) << "excessSize: " << excessSize << endl;
            }
            DiskLoc startLoc = extent.ext()->firstRecord;

            CursorId id;
            {
                auto_ptr< Cursor > c = theDataFileMgr.findAll( fromNs.c_str(), startLoc );
                ClientCursor *cc = new ClientCursor(c, fromNs.c_str(), true);
                cc->matcher.reset( new CoveredIndexMatcher( BSONObj(), fromjson( "{$natural:1}" ) ) );
                id = cc->cursorid;
            }

            DBDirectClient client;
            Client::Context ctx( toNs );
            BSONObjBuilder spec;
            spec.appendBool( "capped", true );
            spec.append( "size", double( size ) );
            if ( !userCreateNS( toNs.c_str(), spec.done(), errmsg, true ) )
                return false;

            auto_ptr< DBClientCursor > c = client.getMore( fromNs, id );
            while( c->more() ) {
                BSONObj obj = c->next();
                theDataFileMgr.insertAndLog( toNs.c_str(), obj, true );
            }

            return true;
        }
    } cmdCloneCollectionAsCapped;

    /* jan2010: 
       Converts the given collection to a capped collection w/ the specified size. 
       This command is not highly used, and is not currently supported with sharded 
       environments. 
       */
    class CmdConvertToCapped : public Command {
    public:
        CmdConvertToCapped() : Command( "convertToCapped" ) {}
        virtual bool slaveOk() { return false; }
        virtual LockType locktype(){ return WRITE; } 
        virtual void help( stringstream &help ) const {
            help << "example: { convertToCapped:<fromCollectionName>, size:<sizeInBytes> }";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            BackgroundOperation::assertNoBgOpInProgForDb(dbname);

            string from = jsobj.getStringField( "convertToCapped" );
            long long size = (long long)jsobj.getField( "size" ).number();

            if ( from.empty() || size == 0 ) {
                errmsg = "invalid command spec";
                return false;
            }

            char realDbName[256];
            nsToDatabase( dbname, realDbName );

            DBDirectClient client;
            client.dropCollection( string( realDbName ) + "." + from + ".$temp_convertToCapped" );

            BSONObj info;
            if ( !client.runCommand( realDbName,
                                    BSON( "cloneCollectionAsCapped" << from << "toCollection" << ( from + ".$temp_convertToCapped" ) << "size" << double( size ) ),
                                    info ) ) {
                errmsg = "cloneCollectionAsCapped failed: " + string(info);
                return false;
            }

            if ( !client.dropCollection( string( realDbName ) + "." + from ) ) {
                errmsg = "failed to drop original collection";
                return false;
            }

            if ( !client.runCommand( "admin",
                                    BSON( "renameCollection" << ( string( realDbName ) + "." + from + ".$temp_convertToCapped" ) << "to" << ( string( realDbName ) + "." + from ) ),
                                    info ) ) {
                errmsg = "renameCollection failed: " + string(info);
                return false;
            }

            return true;
        }
    } cmdConvertToCapped;

    class GroupCommand : public Command {
    public:
        GroupCommand() : Command("group"){}
        virtual LockType locktype(){ return READ; } 
        virtual bool slaveOk() { return true; }
        virtual void help( stringstream &help ) const {
            help << "see http://www.mongodb.org/display/DOCS/Aggregation";
        }

        BSONObj getKey( const BSONObj& obj , const BSONObj& keyPattern , ScriptingFunction func , double avgSize , Scope * s ){
            if ( func ){
                BSONObjBuilder b( obj.objsize() + 32 );
                b.append( "0" , obj );
                int res = s->invoke( func , b.obj() );
                uassert( 10041 ,  (string)"invoke failed in $keyf: " + s->getError() , res == 0 );
                int type = s->type("return");
                uassert( 10042 ,  "return of $key has to be an object" , type == Object );
                return s->getObject( "return" );
            }
            return obj.extractFields( keyPattern , true );
        }

        bool group( string realdbname , auto_ptr<DBClientCursor> cursor ,
                    BSONObj keyPattern , string keyFunctionCode , string reduceCode , const char * reduceScope ,
                    BSONObj initial , string finalize ,
                    string& errmsg , BSONObjBuilder& result ){


            auto_ptr<Scope> s = globalScriptEngine->getPooledScope( realdbname );
            s->localConnect( realdbname.c_str() );

            if ( reduceScope )
                s->init( reduceScope );

            s->setObject( "$initial" , initial , true );

            s->exec( "$reduce = " + reduceCode , "reduce setup" , false , true , true , 100 );
            s->exec( "$arr = [];" , "reduce setup 2" , false , true , true , 100 );
            ScriptingFunction f = s->createFunction(
                "function(){ "
                "  if ( $arr[n] == null ){ "
                "    next = {}; "
                "    Object.extend( next , $key ); "
                "    Object.extend( next , $initial , true ); "
                "    $arr[n] = next; "
                "    next = null; "
                "  } "
                "  $reduce( obj , $arr[n] ); "
                "}" );

            ScriptingFunction keyFunction = 0;
            if ( keyFunctionCode.size() ){
                keyFunction = s->createFunction( keyFunctionCode.c_str() );
            }


            double keysize = keyPattern.objsize() * 3;
            double keynum = 1;

            map<BSONObj,int,BSONObjCmp> map;
            list<BSONObj> blah;

            while ( cursor->more() ){
                BSONObj obj = cursor->next();
                BSONObj key = getKey( obj , keyPattern , keyFunction , keysize / keynum , s.get() );
                keysize += key.objsize();
                keynum++;

                int& n = map[key];
                if ( n == 0 ){
                    n = map.size();
                    s->setObject( "$key" , key , true );

                    uassert( 10043 ,  "group() can't handle more than 10000 unique keys" , n <= 10000 );
                }

                s->setObject( "obj" , obj , true );
                s->setNumber( "n" , n - 1 );
                if ( s->invoke( f , BSONObj() , 0 , true ) ){
                    throw UserException( 9010 , (string)"reduce invoke failed: " + s->getError() );
                }
            }

            if (!finalize.empty()){
                s->exec( "$finalize = " + finalize , "finalize define" , false , true , true , 100 );
                ScriptingFunction g = s->createFunction(
                    "function(){ "
                    "  for(var i=0; i < $arr.length; i++){ "
                    "  var ret = $finalize($arr[i]); "
                    "  if (ret !== undefined) "
                    "    $arr[i] = ret; "
                    "  } "
                    "}" );
                s->invoke( g , BSONObj() , 0 , true );
            }
            
            result.appendArray( "retval" , s->getObject( "$arr" ) );
            result.append( "count" , keynum - 1 );
            result.append( "keys" , (int)(map.size()) );
            s->exec( "$arr = [];" , "reduce setup 2" , false , true , true , 100 );
            s->gc();

            return true;
        }

        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            static DBDirectClient db;

            /* db.$cmd.findOne( { group : <p> } ) */
            const BSONObj& p = jsobj.firstElement().embeddedObjectUserCheck();

            BSONObj q;
            if ( p["cond"].type() == Object )
                q = p["cond"].embeddedObject();
            else if ( p["condition"].type() == Object )
                q = p["condition"].embeddedObject();
            else 
                q = getQuery( p );

            string ns = dbname;
            ns = ns.substr( 0 , ns.size() - 4 );
            string realdbname = ns.substr( 0 , ns.size() - 1 );

            if ( p["ns"].type() != String ){
                errmsg = "ns has to be set";
                return false;
            }

            ns += p["ns"].valuestr();

            auto_ptr<DBClientCursor> cursor = db.query( ns , q );

            BSONObj key;
            string keyf;
            if ( p["key"].type() == Object ){
                key = p["key"].embeddedObjectUserCheck();
                if ( ! p["$keyf"].eoo() ){
                    errmsg = "can't have key and $keyf";
                    return false;
                }
            }
            else if ( p["$keyf"].type() ){
                keyf = p["$keyf"].ascode();
            }
            else {
                // no key specified, will use entire object as key
            }

            BSONElement reduce = p["$reduce"];
            if ( reduce.eoo() ){
                errmsg = "$reduce has to be set";
                return false;
            }

            BSONElement initial = p["initial"];
            if ( initial.type() != Object ){
                errmsg = "initial has to be an object";
                return false;
            }


            string finalize;
            if (p["finalize"].type())
                finalize = p["finalize"].ascode();

            return group( realdbname , cursor ,
                          key , keyf , reduce.ascode() , reduce.type() != CodeWScope ? 0 : reduce.codeWScopeScopeData() ,
                          initial.embeddedObject() , finalize ,
                          errmsg , result );
        }

    } cmdGroup;


    class DistinctCommand : public Command {
    public:
        DistinctCommand() : Command("distinct"){}
        virtual bool slaveOk() { return true; }
        virtual LockType locktype(){ return READ; } 
        virtual void help( stringstream &help ) const {
            help << "{ distinct : 'collection name' , key : 'a.b' }";
        }

        bool run(const char *dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            static DBDirectClient db;

            string ns = cc().database()->name + '.' + cmdObj.getField(name).valuestr();
            string key = cmdObj["key"].valuestrsafe();

            BSONObj keyPattern = BSON( key << 1 );

            set<BSONObj,BSONObjCmp> map;

            long long size = 0;

            auto_ptr<DBClientCursor> cursor = db.query( ns , getQuery( cmdObj ) , 0 , 0 , &keyPattern );
            while ( cursor->more() ){
                BSONObj o = cursor->next();
                BSONObj value = o.extractFields( keyPattern );
                if ( value.isEmpty() )
                    continue;
                if ( map.insert( value ).second ){
                    size += o.objsize() + 20;
                    uassert( 10044 ,  "distinct too big, 4mb cap" , size < 4 * 1024 * 1024 );
                }
            }

            assert( size <= 0x7fffffff );
            BSONObjBuilder b( (int) size );
            int n=0;
            for ( set<BSONObj,BSONObjCmp>::iterator i = map.begin() ; i != map.end(); i++ ){
                b.appendAs( i->firstElement() , b.numStr( n++ ).c_str() );
            }

            result.appendArray( "values" , b.obj() );

            return true;
        }

    } distinctCmd;

    /* Find and Modify an object returning either the old (default) or new value*/
    class CmdFindAndModify : public Command {
    public:
        /* {findandmodify: "collection", query: {processed:false}, update: {$set: {processed:true}}, new: true}
         * {findandmodify: "collection", query: {processed:false}, remove: true, sort: {priority:-1}}
         * 
         * either update or remove is required, all other fields have default values
         * output is in the "value" field
         */
        CmdFindAndModify() : Command("findandmodify") { }
        virtual bool logTheOp() {
            return false; // the modification will be logged directly
        }
        virtual bool slaveOk() {
            return false;
        }
        virtual LockType locktype(){ return WRITE; } 
        virtual bool run(const char *dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            static DBDirectClient db;

            string ns = nsToDatabase(dbname) + '.' + cmdObj.firstElement().valuestr();

            Query q (cmdObj.getObjectField("query")); // defaults to {}
            BSONElement sort = cmdObj["sort"];
            if (!sort.eoo())
                q.sort(sort.embeddedObjectUserCheck());

            BSONObj out = db.findOne(ns, q);
            if (out.firstElement().eoo()){
                errmsg = "No matching object found";
                return false;
            }

            q = QUERY( "_id" << out["_id"]);

            if (cmdObj["remove"].trueValue()){
                uassert(12515, "can't remove and update", cmdObj["update"].eoo());
                db.remove(ns, q, 1);
            } else {
                BSONElement update = cmdObj["update"];
                uassert(12516, "must specify remove or update", !update.eoo());
                db.update(ns, q, update.embeddedObjectUserCheck());

                if (cmdObj["new"].trueValue())
                    out = db.findOne(ns, q);
            }

            result.append("value", out);

            return true;
        }
    } cmdFindAndModify;
    
    /* Returns client's uri */
    class CmdWhatsMyUri : public Command {
    public:
        CmdWhatsMyUri() : Command("whatsmyuri") { }
        virtual bool logTheOp() {
            return false; // the modification will be logged directly
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual LockType locktype(){ return NONE; } 
        virtual bool requiresAuth() {
            return false;
        }
        virtual void help( stringstream &help ) const {
            help << "{whatsmyuri:1}";
        }        
        virtual bool run(const char *dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            BSONObj info = cc().curop()->infoNoauth();
            result << "you" << info[ "client" ];
            return true;
        }
    } cmdWhatsMyUri;
    
    /* For testing only, not for general use */
    class GodInsert : public Command {
    public:
        GodInsert() : Command( "godinsert" ) { }
        virtual bool logTheOp() {
            return true;
        }
        virtual bool slaveOk() {
            return false;
        }
        virtual LockType locktype() { return WRITE; }
        virtual bool requiresAuth() {
            return true;
        }
        virtual void help( stringstream &help ) const {
            help << "[for testing only]";
        }        
        virtual bool run(const char *dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            string coll = cmdObj[ "godinsert" ].valuestrsafe();
            uassert( 13049, "godinsert must specify a collection", !coll.empty() );
            string ns = nsToDatabase( dbname ) + "." + coll;
            BSONObj obj = cmdObj[ "obj" ].embeddedObjectUserCheck();
            DiskLoc loc = theDataFileMgr.insert( ns.c_str(), obj, true );
            return true;
        }
    } cmdGodInsert;

    class DBHashCmd : public Command {
    public:
        DBHashCmd() : Command( "dbhash" ){}
        virtual bool slaveOk() { return true; }
        virtual LockType locktype() { return READ; }
        virtual bool run(const char * badns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            string dbname = nsToDatabase( badns );
            
            list<string> colls = _db.getCollectionNames( dbname );
            colls.sort();
            
            result.appendNumber( "numCollections" , (long long)colls.size() );
            
            md5_state_t globalState;
            md5_init(&globalState);

            BSONObjBuilder bb( result.subobjStart( "collections" ) );
            for ( list<string>::iterator i=colls.begin(); i != colls.end(); i++ ){
                string c = *i;
                if ( c.find( ".system.profil" ) != string::npos )
                    continue;
                
                auto_ptr<Cursor> cursor;

                NamespaceDetails * nsd = nsdetails( c.c_str() );
                int idNum = nsd->findIdIndex();
                if ( idNum >= 0 ){
                    cursor.reset( new BtreeCursor( nsd , idNum , nsd->idx( idNum ) , BSONObj() , BSONObj() , false , 1 ) );
                }
                else if ( c.find( ".system." ) != string::npos ){
                    continue;
                }
                else if ( nsd->capped ){
                    cursor = findTableScan( c.c_str() , BSONObj() );
                }
                else {
                    bb.done();
                    errmsg = (string)"can't find _id index for: " + c;
                    return 0;
                }

                md5_state_t st;
                md5_init(&st);
                
                long long n = 0;
                while ( cursor->ok() ){
                    BSONObj c = cursor->current();
                    md5_append( &st , (const md5_byte_t*)c.objdata() , c.objsize() );
                    n++;
                    cursor->advance();
                }
                md5digest d;
                md5_finish(&st, d);
                string hash = digestToString( d );
                
                bb.append( c.c_str() + ( dbname.size() + 1 ) , hash );

                md5_append( &globalState , (const md5_byte_t*)hash.c_str() , hash.size() );
            }
            bb.done();

            md5digest d;
            md5_finish(&globalState, d);
            string hash = digestToString( d );

            result.append( "md5" , hash );

            return 1;
        }

        DBDirectClient _db;
    } dbhashCmd;
    
    /** 
     * this handles
     - auth
     - locking
     - context
     then calls run()
    */
    bool execCommand( Command * c ,
                      Client& client , int queryOptions , 
                      const char *ns, BSONObj& cmdObj , 
                      BSONObjBuilder& result, 
                      bool fromRepl ){
        
        string dbname = nsToDatabase( ns );
        
        AuthenticationInfo *ai = client.getAuthenticationInfo();    

        if( c->adminOnly() && c->localHostOnlyIfNoAuth( cmdObj ) && noauth && !ai->isLocalHost ) { 
            result.append( "errmsg" , 
                           "unauthorized: this command must run from localhost when running db without auth" );
            log() << "command denied: " << cmdObj.toString() << endl;
            return false;
        }
        

        if ( c->adminOnly() && ! fromRepl && dbname != "admin" ) {
            result.append( "errmsg" ,  "access denied" );
            log() << "command denied: " << cmdObj.toString() << endl;
            return false;
        }        

        if ( cmdObj["help"].trueValue() ){
            stringstream ss;
            ss << "help for: " << c->name << " ";
            c->help( ss );
            result.append( "help" , ss.str() );
            result.append( "lockType" , c->locktype() );
            return true;
        } 

        bool canRunHere = 
            isMaster( dbname.c_str() ) ||
            c->slaveOk() ||
            ( c->slaveOverrideOk() && ( queryOptions & QueryOption_SlaveOk ) ) ||
            fromRepl;

        if ( ! canRunHere ){
            result.append( "errmsg" , "not master" );
            return false;
        }
        
        if ( c->locktype() == Command::NONE ){
            // we also trust that this won't crash
            string errmsg;
            int ok = c->run( ns , cmdObj , errmsg , result , fromRepl );
            if ( ! ok )
                result.append( "errmsg" , errmsg );
            return ok;
        }
     
        bool needWriteLock = c->locktype() == Command::WRITE;
        
        if ( ! c->requiresAuth() && 
             ( ai->isAuthorizedReads( dbname ) && 
               ! ai->isAuthorized( dbname ) ) ){
            // this means that they can read, but not write
            // so only get a read lock
            needWriteLock = false;
        }
        
        if ( ! needWriteLock ){
            assert( ! c->logTheOp() );
        }

        mongolock lk( needWriteLock );
        Client::Context ctx( ns , dbpath , &lk , c->requiresAuth() );
        
        if ( c->adminOnly() )
            log( 2 ) << "command: " << cmdObj << endl;
        
        try {
            string errmsg;
            if ( ! c->run(ns, cmdObj, errmsg, result, fromRepl ) ){
                result.append( "errmsg" , errmsg );
                return false;
            }
        }
        catch ( AssertionException& e ){
            stringstream ss;
            ss << "assertion: " << e.what();
            result.append( "errmsg" , ss.str() );
            return false;
        }
        
        if ( c->logTheOp() && ! fromRepl ){
            logOp("c", ns, cmdObj);
        }
        
        return true;
    }


    /* TODO make these all command objects -- legacy stuff here

       usage:
         abc.$cmd.findOne( { ismaster:1 } );

       returns true if ran a cmd
    */
    bool _runCommands(const char *ns, BSONObj& _cmdobj, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        string dbname = nsToDatabase( ns );

        if( logLevel >= 1 ) 
            log() << "run command " << ns << ' ' << _cmdobj << endl;
        
        const char *p = strchr(ns, '.');
        if ( !p ) return false;
        if ( strcmp(p, ".$cmd") != 0 ) return false;

        BSONObj jsobj;
        {
            BSONElement e = _cmdobj.firstElement();
            if ( e.type() == Object && string("query") == e.fieldName() ) {
                jsobj = e.embeddedObject();
            }
            else {
                jsobj = _cmdobj;
            }
        }

        Client& client = cc();
        bool ok = false;

        BSONElement e = jsobj.firstElement();
        
        Command * c = e.type() ? Command::findCommand( e.fieldName() ) : 0;
        if ( c ){
            ok = execCommand( c , client , queryOptions , ns , jsobj , anObjBuilder , fromRepl );
        }
        else {
            anObjBuilder.append("errmsg", "no such cmd");
            anObjBuilder.append("bad cmd" , _cmdobj );
        }
        anObjBuilder.append("ok", ok?1.0:0.0);
        BSONObj x = anObjBuilder.done();
        b.append((void*) x.objdata(), x.objsize());
        return true;
    }

} // namespace mongo
