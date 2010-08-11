// @file 

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

#pragma once

#include <map>
#include "../../client/dbclient.h"

namespace mongo { 

    /** here we keep a single connection (with reconnect) for a set of hosts, 
        one each, and allow one user at a time per host.  if in use already for that 
        host, we block.  so this is an easy way to keep a 1-deep pool of connections
        that many threads can share.

        thread-safe.

        Example:
        {
            ScopedConn c("foo.acme.com:9999");
            c->runCommand(...);
        }

        throws exception on connect error (but fine to try again later with a new
        scopedconn object for same host).
    */
    class ScopedConn { 
    public:
        /** throws assertions if connect failure etc. */
        ScopedConn(string hostport);
        ~ScopedConn();
        DBClientConnection* operator->();
    private:
        auto_ptr<scoped_lock> connLock;
        static mutex mapMutex;
        struct X { 
            mutex z;
            DBClientConnection cc;
            X() : z("X"), cc(/*reconnect*/true, 0, /*timeout*/10) { 
                cc._logLevel = 2;
            }
        } *x;
        typedef map<string,ScopedConn::X*> M;
        static M& _map;
    };

    inline ScopedConn::ScopedConn(string hostport) {
        bool first = false;
        {
            scoped_lock lk(mapMutex);
            x = _map[hostport];
            if( x == 0 ) {
                x = _map[hostport] = new X();
                first = true;
                connLock.reset( new scoped_lock(x->z) );
            }
        }
        if( !first ) { 
            connLock.reset( new scoped_lock(x->z) );
            return;
        }

        // we already locked above...
        string err;
        x->cc.connect(hostport, err);
    }

    inline ScopedConn::~ScopedConn() { 
        // conLock releases...
    }

    inline DBClientConnection* ScopedConn::operator->() { 
        return &x->cc; 
    }

}
