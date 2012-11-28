// introspect.cpp

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

#include "mongo/pch.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/curop.h"
#include "mongo/db/databaseholder.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"
#include "mongo/util/goodies.h"

namespace mongo {

    static void _profile(const Client& c, CurOp& currentOp, BufBuilder& profileBufBuilder) {
        Database *db = c.database();
        DEV verify( db );
        const char *ns = db->profileName.c_str();
        
        // build object
        BSONObjBuilder b(profileBufBuilder);
        b.appendDate("ts", jsTime());
        currentOp.debug().append( currentOp , b );

        b.append("client", c.clientAddress() );

        if ( c.getAuthenticationInfo() )
            b.append( "user" , c.getAuthenticationInfo()->getUser( nsToDatabase( ns ) ) );

        BSONObj p = b.done();

        if (p.objsize() > 100*1024){
            string small = p.toString(/*isArray*/false, /*full*/false);

            warning() << "can't add full line to system.profile: " << small;

            // rebuild with limited info
            BSONObjBuilder b(profileBufBuilder);
            b.appendDate("ts", jsTime());
            b.append("client", c.clientAddress() );
            if ( c.getAuthenticationInfo() )
                b.append( "user" , c.getAuthenticationInfo()->getUser( nsToDatabase( ns ) ) );

            b.append("err", "profile line too large (max is 100KB)");
            if (small.size() < 100*1024){ // should be much smaller but if not don't break anything
                b.append("abbreviated", small);
            }

            p = b.done();
        }

        // write: not replicated
        // get or create the profiling collection
        NamespaceDetails *details = getOrCreateProfileCollection(db);
        if (details) {
            int len = p.objsize();
            Record *r = theDataFileMgr.fast_oplog_insert(details, ns, len);
            memcpy(getDur().writingPtr(r->data(), len), p.objdata(), len);
        }
    }

    void profile(const Client& c, int op, CurOp& currentOp) {
        // initialize with 1kb to start, to avoid realloc later
        // doing this outside the dblock to improve performance
        BufBuilder profileBufBuilder(1024);

        try {
            Lock::DBWrite lk( currentOp.getNS() );
            if ( dbHolder()._isLoaded( nsToDatabase( currentOp.getNS() ) , dbpath ) ) {
                Client::Context cx( currentOp.getNS(), dbpath, false );
                _profile(c, currentOp, profileBufBuilder);
            }
            else {
                mongo::log() << "note: not profiling because db went away - probably a close on: "
                             << currentOp.getNS() << endl;
            }
        }
        catch (const AssertionException& assertionEx) {
            warning() << "Caught Assertion while trying to profile " << opToString(op)
                      << " against " << currentOp.getNS()
                      << ": " << assertionEx.toString() << endl;
        }
    }

    NamespaceDetails* getOrCreateProfileCollection(Database *db, bool force) {
        fassert(16372, db);
        const char* profileName = db->profileName.c_str();
        NamespaceDetails* details = db->namespaceIndex.details(profileName);
        if (!details && (cmdLine.defaultProfile || force)) {
            // system.profile namespace doesn't exist; create it
            log() << "creating profile collection: " << profileName << endl;
            string errmsg;
            if (!userCreateNS(db->profileName.c_str(),
                              BSON("capped" << true << "size" << 1024 * 1024), errmsg , false)) {
                log() << "could not create ns " << db->profileName << ": " << errmsg << endl;
                return NULL;
            }
            details = db->namespaceIndex.details(profileName);
        }
        if (!details) {
            // failed to get or create profile collection
            static time_t last = time(0) - 10;  // warn the first time
            if( time(0) > last+10 ) {
                log() << "profile: warning ns " << profileName << " does not exist" << endl;
                last = time(0);
            }
        }
        return details;
    }

} // namespace mongo
