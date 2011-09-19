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

#pragma once

#include "db.h"
#include "dbhelpers.h"
#include "json.h"
#include "../client/dbclient.h"
#include "repl.h"
#include "cmdline.h"
#include "repl/rs.h"
#include "ops/query.h"

namespace mongo {

    extern const char *replAllDead;

    /* note we always return true for the "local" namespace.

       we should not allow most operations when not the master
       also we report not master if we are "dead".

       See also CmdIsMaster.

       If 'client' is not specified, the current client is used.
    */
    inline bool _isMaster() {
        if( replSet ) {
            if( theReplSet )
                return theReplSet->isPrimary();
            return false;
        }

        if( ! replSettings.slave )
            return true;

        if ( replAllDead )
            return false;

        if( replSettings.master ) {
            // if running with --master --slave, allow.
            return true;
        }

        if ( cc().isGod() )
            return true;

        return false;
    }
    inline bool isMaster(const char *client = 0) {
        if( _isMaster() )
            return true;
        if ( !client ) {
            Database *database = cc().database();
            assert( database );
            client = database->name.c_str();
        }
        return strcmp( client, "local" ) == 0;
    }

    inline void notMasterUnless(bool expr) {
        uassert( 10107 , "not master" , expr );
    }

    /** we allow queries to SimpleSlave's */
    inline void replVerifyReadsOk(ParsedQuery& pq) {
        if( replSet ) {
            /* todo: speed up the secondary case.  as written here there are 2 mutex entries, it can b 1. */
            if( isMaster() ) return;
            uassert(13435, "not master and slaveok=false", pq.hasOption(QueryOption_SlaveOk));
            uassert(13436, "not master or secondary, can't read", theReplSet && theReplSet->isSecondary() );
        }
        else {
            notMasterUnless(isMaster() || pq.hasOption(QueryOption_SlaveOk) || replSettings.slave == SimpleSlave );
        }
    }

    inline bool isMasterNs( const char *ns ) {
        char cl[ 256 ];
        nsToDatabase( ns, cl );
        return isMaster( cl );
    }

} // namespace mongo
