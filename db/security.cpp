// security.cpp

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

#include "pch.h"
#include "security.h"
#include "security_common.h"
#include "instance.h"
#include "client.h"
#include "curop-inl.h"
#include "db.h"
#include "dbhelpers.h"

// this is the _mongod only_ implementation of security.h

namespace mongo {

    bool AuthenticationInfo::_warned = false;
    /*
    void AuthenticationInfo::print() const {
        cout << "AuthenticationInfo: " << this << '\n';
        for ( MA::const_iterator i=_dbs.begin(); i!=_dbs.end(); i++ ) {
            cout << "\t" << i->first << "\t" << i->second.level << '\n';
        }
        cout << "END" << endl;
    }
    */

    string AuthenticationInfo::getUser( const string& dbname ) const {
        scoped_spinlock lk(_lock);

        MA::const_iterator i = _dbs.find(dbname);
        if ( i == _dbs.end() )
            return "";

        return i->second.user;
    }


    bool AuthenticationInfo::_isAuthorizedSpecialChecks( const string& dbname ) const {
        if ( cc().isGod() ) 
            return true;

        if ( isLocalHost ) {
            atleastreadlock l("");
            Client::GodScope gs;
            Client::Context c("admin.system.users");
            BSONObj result;
            if( ! Helpers::getSingleton("admin.system.users", result) ) {
                if( ! _warned ) {
                    // you could get a few of these in a race, but that's ok
                    _warned = true;
                    log() << "note: no users configured in admin.system.users, allowing localhost access" << endl;
                }
                return true;
            }
        }

        return false;
    }

    bool CmdAuthenticate::getUserObj(const string& dbname, const string& user, BSONObj& userObj, string& pwd) {
        if (user == internalSecurity.user) {
            uassert(15889, "key file must be used to log in with internal user", cmdLine.keyFile);
            pwd = internalSecurity.pwd;
        }
        else {
            // static BSONObj userPattern = fromjson("{\"user\":1}");
            string systemUsers = dbname + ".system.users";
            // OCCASIONALLY Helpers::ensureIndex(systemUsers.c_str(), userPattern, false, "user_1");
            {
                mongolock lk(false);
                Client::Context c(systemUsers, dbpath, &lk, false);

                BSONObjBuilder b;
                b << "user" << user;
                BSONObj query = b.done();
                if( !Helpers::findOne(systemUsers.c_str(), query, userObj) ) {
                    log() << "auth: couldn't find user " << user << ", " << systemUsers << endl;
                    return false;
                }
            }

            pwd = userObj.getStringField("pwd");
        }
        return true;
    }

    void CmdAuthenticate::authenticate(const string& dbname, const string& user, const bool readOnly) {
        AuthenticationInfo *ai = cc().getAuthenticationInfo();

        if ( readOnly ) {
            ai->authorizeReadOnly( dbname.c_str() , user );
        }
        else {
            ai->authorize( dbname.c_str() , user );
        }
    }

    bool CmdLogout::run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        AuthenticationInfo *ai = cc().getAuthenticationInfo();
        ai->logout(dbname);
        return true;
    }

} // namespace mongo

