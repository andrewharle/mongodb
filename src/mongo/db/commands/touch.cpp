/** @file touch.cpp
    compaction of deleted space in pdfiles (datafiles)
*/

/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,b
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/pch.h"

#include <string>
#include <vector>

#include "mongo/db/kill_current_op.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/structure/catalog/index_details.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/util/timer.h"
#include "mongo/util/touch_pages.h"

namespace mongo {

    struct touch_location {
        const char* root;
        size_t length;
    };

    /** @return numRanges touched */
    int touchNs( const std::string& ns ) {
        std::vector< touch_location > ranges;
        boost::scoped_ptr<LockMongoFilesShared> mongoFilesLock;
        {
            Client::ReadContext ctx(ns);

            Database* db = ctx.ctx().db();
            ExtentManager& em = db->getExtentManager();

            Collection* collection = db->getCollection( ns );
            uassert( 16154, "namespace does not exist", collection );

            Extent* ext = em.getExtent( collection->details()->firstExtent() );
            while ( ext ) {
                touch_location tl;
                tl.root = reinterpret_cast<const char*>(ext);
                tl.length = ext->length;
                ranges.push_back(tl);
                ext = em.getNextExtent( ext );
            }
            mongoFilesLock.reset(new LockMongoFilesShared());
        }
        // DB read lock is dropped; no longer needed after this point.

        std::string progress_msg = "touch " + ns + " extents";
        ProgressMeterHolder pm(cc().curop()->setMessage(progress_msg.c_str(),
                                                        "Touch Progress",
                                                        ranges.size()));
        for ( std::vector< touch_location >::iterator it = ranges.begin(); it != ranges.end(); ++it ) {
            touch_pages( it->root, it->length );
            pm.hit();
            killCurrentOp.checkForInterrupt();
        }
        pm.finished();

        return static_cast<int>( ranges.size() );
    }

    class TouchCmd : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool maintenanceMode() const { return true; }
        virtual bool logTheOp() { return false; }
        virtual void help( stringstream& help ) const {
            help << "touch collection\n"
                "Page in all pages of memory containing every extent for the given collection\n"
                "{ touch : <collection_name>, [data : true] , [index : true] }\n"
                " at least one of data or index must be true; default is both are false\n";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::touch);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        TouchCmd() : Command("touch") { }

        virtual bool run(const string& db, 
                         BSONObj& cmdObj, 
                         int, 
                         string& errmsg, 
                         BSONObjBuilder& result, 
                         bool fromRepl) {
            string coll = cmdObj.firstElement().valuestr();
            if( coll.empty() || db.empty() ) {
                errmsg = "no collection name specified";
                return false;
            }
                        
            string ns = db + '.' + coll;
            if ( ! NamespaceString::normal(ns.c_str()) ) {
                errmsg = "bad namespace name";
                return false;
            }

            bool touch_indexes( cmdObj["index"].trueValue() );
            bool touch_data( cmdObj["data"].trueValue() );

            if ( ! (touch_indexes || touch_data) ) {
                errmsg = "must specify at least one of (data:true, index:true)";
                return false;
            }
            bool ok = touch( ns, errmsg, touch_data, touch_indexes, result );
            return ok;
        }

        bool touch( std::string& ns, 
                    std::string& errmsg, 
                    bool touch_data, 
                    bool touch_indexes, 
                    BSONObjBuilder& result ) {

            if (touch_data) {
                log() << "touching namespace " << ns << endl;
                Timer t;
                int numRanges = touchNs( ns );
                result.append( "data", BSON( "numRanges" << numRanges <<
                                             "millis" << t.millis() ) );
                log() << "touching namespace " << ns << " complete" << endl;
            }

            if (touch_indexes) {
                Timer t;
                // enumerate indexes
                std::vector< std::string > indexes;
                {
                    Client::ReadContext ctx(ns);
                    NamespaceDetails *nsd = nsdetails(ns);
                    massert( 16153, "namespace does not exist", nsd );

                    NamespaceDetails::IndexIterator ii = nsd->ii(); 
                    while ( ii.more() ) {
                        IndexDetails& idx = ii.next();
                        indexes.push_back( idx.indexNamespace() );
                    }
                }

                int numRanges = 0;

                for ( std::vector<std::string>::const_iterator it = indexes.begin(); 
                      it != indexes.end(); 
                      it++ ) {
                    numRanges += touchNs( *it );
                }

                result.append( "indexes", BSON( "num" << static_cast<int>(indexes.size()) <<
                                                "numRanges" << numRanges <<
                                                "millis" << t.millis() ) );

            }
            return true;
        }
        
    };
    static TouchCmd touchCmd;
}
