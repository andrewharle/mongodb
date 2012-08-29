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

    void AuthenticationInfo::setTemporaryAuthorization( BSONObj& obj ) {
        fassert( 16232, !_usingTempAuth );
        scoped_spinlock lk( _lock );
        LOG(5) << "Setting temporary authorization to: " << obj << endl;
        _tempAuthTable.setFromBSON( obj );
        _usingTempAuth = true;
    }

    void AuthenticationInfo::clearTemporaryAuthorization() {
        scoped_spinlock lk( _lock );
        _usingTempAuth = false;
        _tempAuthTable.clearAuth();
    }

    bool AuthenticationInfo::hasTemporaryAuthorization() {
        scoped_spinlock lk( _lock );
        return _usingTempAuth;
    }

    bool AuthenticationInfo::usingInternalUser() {
        return getUser("local") == internalSecurity.user ||
            getUser("admin") == internalSecurity.user;
    }

    string AuthenticationInfo::getUser( const string& dbname ) const {
        scoped_spinlock lk(_lock);
        return _authTable.getAuthForDb( dbname ).user;
    }

    bool AuthenticationInfo::_isAuthorizedSpecialChecks( const string& dbname ) const {
        if ( cc().isGod() ) 
            return true;
        return _isLocalHostAndLocalHostIsAuthorizedForAll;
    }

    void AuthenticationInfo::setIsALocalHostConnectionWithSpecialAuthPowers() {
        verify(!_isLocalHost);
        _isLocalHost = true;
        _isLocalHostAndLocalHostIsAuthorizedForAll = true;
        _checkLocalHostSpecialAdmin();
    }

    void AuthenticationInfo::_checkLocalHostSpecialAdmin() {
        if ( ! _isLocalHost )
            return;
        
        if ( ! _isLocalHostAndLocalHostIsAuthorizedForAll )
            return;
        
        if ( ! noauth ) {
            Client::GodScope gs;
            Client::ReadContext ctx("admin.system.users");
            BSONObj result;
            if( Helpers::getSingleton("admin.system.users", result) ) {
                _isLocalHostAndLocalHostIsAuthorizedForAll = false;
            }
            else if ( ! _warned ) {
                // you could get a few of these in a race, but that's ok
                _warned = true;
                log() << "note: no users configured in admin.system.users, allowing localhost access" << endl;
            }
        }
        
        
    }

    void AuthenticationInfo::startRequest() {
        if ( ! Lock::isLocked() ) {
            _checkLocalHostSpecialAdmin();
        }
    }

    AuthenticationInfo::TemporaryAuthReleaser::TemporaryAuthReleaser ( AuthenticationInfo* ai ) :
        _ai (ai), _hadTempAuthFromStart( ai->hasTemporaryAuthorization() ) {}

    AuthenticationInfo::TemporaryAuthReleaser::~TemporaryAuthReleaser() {
        // Some commands can run other commands using the DBDirectClient, which leads to the
        // temporary auth already being set when the inner command runs.  If that's the case, we
        // shouldn't clear the temporary auth set by a command higher up in the call stack.
        if ( !_hadTempAuthFromStart ) {
            _ai->clearTemporaryAuthorization();
        }
    }

    bool CmdAuthenticate::getUserObj(const string& dbname, const string& user, BSONObj& userObj, string& pwd) {
        if (user == internalSecurity.user) {
            uassert(15889, "key file must be used to log in with internal user", cmdLine.keyFile);
            pwd = internalSecurity.pwd;
        }
        else {
            string systemUsers = dbname + ".system.users";
            {
                Client::ReadContext ctx( systemUsers , dbpath, false );

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

    bool CmdLogout::run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        AuthenticationInfo *ai = cc().getAuthenticationInfo();
        ai->logout(dbname);
        return true;
    }

} // namespace mongo

