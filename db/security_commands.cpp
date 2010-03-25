// security_commands.cpp
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

// security.cpp links with both dbgrid and db.  this file db only -- at least for now.

// security.cpp

#include "stdafx.h"
#include "security.h"
#include "../util/md5.hpp"
#include "json.h" 
#include "pdfile.h"
#include "db.h"
#include "dbhelpers.h"
#include "commands.h"
#include "jsobj.h"
#include "client.h"

namespace mongo {

/* authentication

   system.users contains 
     { user : <username>, pwd : <pwd_digest>, ... }

   getnonce sends nonce to client

   client then sends { authenticate:1, nonce:<nonce_str>, user:<username>, key:<key> }

   where <key> is md5(<nonce_str><username><pwd_digest_str>) as a string
*/

    boost::thread_specific_ptr<nonce> lastNonce;

    class CmdGetNonce : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual LockType locktype(){ return NONE; }
        CmdGetNonce() : Command("getnonce") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            nonce *n = new nonce(security.getNonce());
            stringstream ss;
            ss << hex << *n;
            result.append("nonce", ss.str() );
            lastNonce.reset(n);
            return true;
        }
    } cmdGetNonce;

    class CmdLogout : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual LockType locktype(){ return NONE; }
        CmdLogout() : Command("logout") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            // database->name is the one we are logging out...
            AuthenticationInfo *ai = cc().getAuthenticationInfo();
            ai->logout(nsToDatabase(ns));
            return true;
        }
    } cmdLogout;
    
    class CmdAuthenticate : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() {
            return true;
        }
        virtual LockType locktype(){ return WRITE; } // TODO: make this READ
        CmdAuthenticate() : Command("authenticate") {}
        bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            log(1) << " authenticate: " << cmdObj << endl;

            string user = cmdObj.getStringField("user");
            string key = cmdObj.getStringField("key");
            string received_nonce = cmdObj.getStringField("nonce");
            
            if( user.empty() || key.empty() || received_nonce.empty() ) { 
                log() << "field missing/wrong type in received authenticate command " 
                    << cc().database()->name
                    << endl;               
                errmsg = "auth fails";
                sleepmillis(10);
                return false;
            }
            
            stringstream digestBuilder;

            {
                bool reject = false;
                nonce *ln = lastNonce.release();
                if ( ln == 0 ) {
                    reject = true;
                } else {
                    digestBuilder << hex << *ln;
                    reject = digestBuilder.str() != received_nonce;
                }
                    
                if ( reject ) {
                    log() << "auth: bad nonce received or getnonce not called. could be a driver bug or a security attack. db:" << cc().database()->name << endl;
                    errmsg = "auth fails";
                    sleepmillis(30);
                    return false;
                }
            }

            static BSONObj userPattern = fromjson("{\"user\":1}");
            string systemUsers = cc().database()->name + ".system.users";
            OCCASIONALLY Helpers::ensureIndex(systemUsers.c_str(), userPattern, false, "user_1");

            BSONObj userObj;
            {
                BSONObjBuilder b;
                b << "user" << user;
                BSONObj query = b.done();
                if( !Helpers::findOne(systemUsers.c_str(), query, userObj) ) { 
                    log() << "auth: couldn't find user " << user << ", " << systemUsers << endl;
                    errmsg = "auth fails";
                    return false;
                }
            }
            
            md5digest d;
            {
                
                string pwd = userObj.getStringField("pwd");
                digestBuilder << user << pwd;
                string done = digestBuilder.str();
                
                md5_state_t st;
                md5_init(&st);
                md5_append(&st, (const md5_byte_t *) done.c_str(), done.size());
                md5_finish(&st, d);
            }
            
            string computed = digestToString( d );
            
            if ( key != computed ){
                log() << "auth: key mismatch " << user << ", ns:" << ns << endl;
                errmsg = "auth fails";
                return false;
            }

            AuthenticationInfo *ai = cc().getAuthenticationInfo();
            
            if ( userObj[ "readOnly" ].isBoolean() && userObj[ "readOnly" ].boolean() ) {
                if ( readLockSupported() ){
                    ai->authorizeReadOnly( cc().database()->name.c_str() );
                }
                else {
                    log() << "warning: old version of boost, read-only users not supported" << endl;
                    ai->authorize( cc().database()->name.c_str() );
                }
            } else {
                ai->authorize( cc().database()->name.c_str() );
            }
            return true;
        }
    } cmdAuthenticate;
    
} // namespace mongo
