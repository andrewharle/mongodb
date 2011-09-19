// s/client.cpp

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
#include "server.h"

#include "../db/commands.h"
#include "../db/dbmessage.h"
#include "../db/stats/counters.h"

#include "../client/connpool.h"

#include "client.h"
#include "request.h"
#include "config.h"
#include "chunk.h"
#include "stats.h"
#include "cursors.h"
#include "grid.h"
#include "s/writeback_listener.h"

namespace mongo {

    ClientInfo::ClientInfo() {
        _cur = &_a;
        _prev = &_b;
        _autoSplitOk = true;
        newRequest();
    }

    ClientInfo::~ClientInfo() {
    }

    void ClientInfo::addShard( const string& shard ) {
        _cur->insert( shard );
        _sinceLastGetError.insert( shard );
    }

    void ClientInfo::newRequest( AbstractMessagingPort* p ) {

        if ( p ) {
            HostAndPort r = p->remote();
            if ( ! _remote.hasPort() )
                _remote = r;
            else if ( _remote != r ) {
                stringstream ss;
                ss << "remotes don't match old [" << _remote.toString() << "] new [" << r.toString() << "]";
                throw UserException( 13134 , ss.str() );
            }
        }

        _lastAccess = (int) time(0);

        set<string> * temp = _cur;
        _cur = _prev;
        _prev = temp;
        _cur->clear();
    }

    ClientInfo * ClientInfo::get() {
        ClientInfo * info = _tlInfo.get();
        if ( ! info ) {
            info = new ClientInfo();
            _tlInfo.reset( info );
            info->newRequest();
        }
        return info;
    }

    void ClientInfo::disconnect() {
        // should be handled by TL cleanup
        _lastAccess = 0;
    }

    void ClientInfo::_addWriteBack( vector<WBInfo>& all , const BSONObj& gle ) {
        BSONElement w = gle["writeback"];

        if ( w.type() != jstOID )
            return;

        BSONElement cid = gle["connectionId"];

        if ( cid.eoo() ) {
            error() << "getLastError writeback can't work because of version mismatch" << endl;
            return;
        }

        string ident = "";
        if ( gle["instanceIdent"].type() == String )
            ident = gle["instanceIdent"].String();

        all.push_back( WBInfo( WriteBackListener::ConnectionIdent( ident , cid.numberLong() ) , w.OID() ) );
    }

    vector<BSONObj> ClientInfo::_handleWriteBacks( vector<WBInfo>& all , bool fromWriteBackListener ) {
        vector<BSONObj> res;

        if ( all.size() == 0 )
            return res;
        
        if ( fromWriteBackListener ) {
            LOG(1) << "not doing recursive writeback" << endl;
            return res;
        }
        
        for ( unsigned i=0; i<all.size(); i++ ) {
            res.push_back( WriteBackListener::waitFor( all[i].ident , all[i].id ) );
        }

        return res;
    }



    bool ClientInfo::getLastError( const BSONObj& options , BSONObjBuilder& result , bool fromWriteBackListener ) {
        set<string> * shards = getPrev();

        if ( shards->size() == 0 ) {
            result.appendNull( "err" );
            return true;
        }

        vector<WBInfo> writebacks;

        // handle single server
        if ( shards->size() == 1 ) {
            string theShard = *(shards->begin() );

            ShardConnection conn( theShard , "", true );
            
            BSONObj res;
            bool ok = false;
            try{
            	ok = conn->runCommand( "admin" , options , res );
            }
            catch( std::exception &e ){
                
                warning() << "could not get last error." << causedBy( e ) << endl;
                
                // Catch everything that happens here, since we need to ensure we return our connection when we're
            	// finished.
            	conn.done();
                
            	return false;
            }
            
            res = res.getOwned();
            conn.done();
            

            _addWriteBack( writebacks , res );

            // hit other machines just to block
            for ( set<string>::const_iterator i=sinceLastGetError().begin(); i!=sinceLastGetError().end(); ++i ) {
                string temp = *i;
                if ( temp == theShard )
                    continue;

                ShardConnection conn( temp , "" );
                _addWriteBack( writebacks , conn->getLastErrorDetailed() );
                conn.done();
            }
            clearSinceLastGetError();
            
            if ( writebacks.size() ){
                vector<BSONObj> v = _handleWriteBacks( writebacks , fromWriteBackListener );
                if ( v.size() == 0 && fromWriteBackListener ) {
                    // ok
                }
                else {
                    assert( v.size() == 1 );
                    result.appendElements( v[0] );
                    result.appendElementsUnique( res );
                    result.append( "writebackGLE" , v[0] );
                    result.append( "initialGLEHost" , theShard );
                }
            }
            else {
                result.append( "singleShard" , theShard );
                result.appendElements( res );
            }
            
            return ok;
        }

        BSONArrayBuilder bbb( result.subarrayStart( "shards" ) );
        BSONObjBuilder shardRawGLE;

        long long n = 0;
        
        int updatedExistingStat = 0; // 0 is none, -1 has but false, 1 has true

        // hit each shard
        vector<string> errors;
        vector<BSONObj> errorObjects;
        for ( set<string>::iterator i = shards->begin(); i != shards->end(); i++ ) {
            string theShard = *i;
            bbb.append( theShard );
            ShardConnection conn( theShard , "", true );
            BSONObj res;
            bool ok = false;
            try {
                ok = conn->runCommand( "admin" , options , res );
                shardRawGLE.append( theShard , res );
            }
            catch( std::exception &e ){

        	    // Safe to return here, since we haven't started any extra processing yet, just collecting
        	    // responses.
                
        	    warning() << "could not get last error." << causedBy( e ) << endl;
                conn.done();
                
                return false;
            }
            
            _addWriteBack( writebacks, res );
            
            string temp = DBClientWithCommands::getLastErrorString( res );
            if ( conn->type() != ConnectionString::SYNC && ( ok == false || temp.size() ) ) {
                errors.push_back( temp );
                errorObjects.push_back( res );
            }

            n += res["n"].numberLong();
            if ( res["updatedExisting"].type() ) {
                if ( res["updatedExisting"].trueValue() )
                    updatedExistingStat = 1;
                else if ( updatedExistingStat == 0 )
                    updatedExistingStat = -1;
            }

            conn.done();
        }

        bbb.done();
        result.append( "shardRawGLE" , shardRawGLE.obj() );

        result.appendNumber( "n" , n );
        if ( updatedExistingStat )
            result.appendBool( "updatedExisting" , updatedExistingStat > 0 );

        // hit other machines just to block
        for ( set<string>::const_iterator i=sinceLastGetError().begin(); i!=sinceLastGetError().end(); ++i ) {
            string temp = *i;
            if ( shards->count( temp ) )
                continue;

            ShardConnection conn( temp , "" );
            _addWriteBack( writebacks, conn->getLastErrorDetailed() );
            conn.done();
        }
        clearSinceLastGetError();

        if ( errors.size() == 0 ) {
            result.appendNull( "err" );
            _handleWriteBacks( writebacks , fromWriteBackListener );
            return true;
        }

        result.append( "err" , errors[0].c_str() );

        {
            // errs
            BSONArrayBuilder all( result.subarrayStart( "errs" ) );
            for ( unsigned i=0; i<errors.size(); i++ ) {
                all.append( errors[i].c_str() );
            }
            all.done();
        }

        {
            // errObjects
            BSONArrayBuilder all( result.subarrayStart( "errObjects" ) );
            for ( unsigned i=0; i<errorObjects.size(); i++ ) {
                all.append( errorObjects[i] );
            }
            all.done();
        }
        _handleWriteBacks( writebacks , fromWriteBackListener );
        return true;
    }

    boost::thread_specific_ptr<ClientInfo> ClientInfo::_tlInfo;

} // namespace mongo
