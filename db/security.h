// security.h

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

#pragma once

#include "nonce.h"
#include "concurrency.h"
#include "security_common.h"
#include "../util/concurrency/spin_lock.h"

// this is used by both mongos and mongod

namespace mongo {

    /* 
     * for a particular db
     * levels
     *     0 : none
     *     1 : read
     *     2 : write
     */
    struct Auth {
        Auth() { level = 0; }
        int level;
        string user;
    };

    class AuthenticationInfo : boost::noncopyable {
    public:
        bool isLocalHost;
        
        AuthenticationInfo(){ isLocalHost = false; }
        ~AuthenticationInfo() {}

        // -- modifiers ----
        
        void logout(const string& dbname ) {
            scoped_spinlock lk(_lock);
            _dbs.erase(dbname);
        }
        void authorize(const string& dbname , const string& user ) {
            scoped_spinlock lk(_lock);
            _dbs[dbname].level = 2;
            _dbs[dbname].user = user;
        }
        void authorizeReadOnly(const string& dbname , const string& user ) {
            scoped_spinlock lk(_lock);
            _dbs[dbname].level = 1;
            _dbs[dbname].user = user;
        }
        
        // -- accessors ---

        bool isAuthorized(const string& dbname) const { 
            return _isAuthorized( dbname, 2 ); 
        }
        
        bool isAuthorizedReads(const string& dbname) const { 
            return _isAuthorized( dbname, 1 ); 
        }
        
        bool isAuthorizedForLock(const string& dbname, int lockType ) const { 
            return _isAuthorized( dbname , lockType > 0 ? 2 : 1 ); 
        }

        string getUser( const string& dbname ) const;

        void print() const;

    protected:
        /** takes a lock */
        bool _isAuthorized(const string& dbname, int level) const;

        bool _isAuthorizedSingle_inlock(const string& dbname, int level) const;
        
        /** cannot call this locked */
        bool _isAuthorizedSpecialChecks( const string& dbname ) const ;

    private:
        mutable SpinLock _lock;

        typedef map<string,Auth> MA;
        MA _dbs; // dbname -> auth

        static bool _warned;
    };

} // namespace mongo
