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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/pch.h"

#include <boost/thread/thread.hpp>
#include <fstream>
#include <iostream>

#include "mongo/base/init.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/jsobjmanipulator.h"
#include "mongo/db/json.h"
#include "mongo/s/type_shard.h"
#include "mongo/tools/stat_util.h"
#include "mongo/tools/mongostat_options.h"
#include "mongo/tools/tool.h"
#include "mongo/util/net/httpclient.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/text.h"

namespace mongo {

    class Stat : public Tool {
    public:

        Stat() : Tool() {
            _autoreconnect = true;
        }

        virtual void printHelp( ostream & out ) {
            printMongoStatHelp(&out);
        }

        BSONObj stats() {
            if (mongoStatGlobalParams.http) {
                HttpClient c;
                HttpClient::Result r;

                string url;
                {
                    stringstream ss;
                    ss << "http://" << toolGlobalParams.connectionString;
                    if (toolGlobalParams.connectionString.find( ":" ) == string::npos)
                        ss << ":28017";
                    ss << "/_status";
                    url = ss.str();
                }

                if ( c.get( url , &r ) != 200 ) {
                    toolError() << "error (http): " << r.getEntireResponse() << std::endl;
                    return BSONObj();
                }

                BSONObj x = fromjson( r.getBody() );
                BSONElement e = x["serverStatus"];
                if ( e.type() != Object ) {
                    toolError() << "BROKEN: " << x << std::endl;
                    return BSONObj();
                }
                return e.embeddedObjectUserCheck();
            }
            BSONObj out;
            if (!conn().simpleCommand(toolGlobalParams.db, &out, "serverStatus")) {
                toolError() << "error: " << out << std::endl;
                return BSONObj();
            }
            return out.getOwned();
        }

        int run() {
            _statUtil.setAll(mongoStatGlobalParams.allFields);
            _statUtil.setSeconds(mongoStatGlobalParams.sleep);
            if (mongoStatGlobalParams.many)
                return runMany();
            return runNormal();
        }

        static void printHeaders( const BSONObj& o ) {
            BSONObjIterator i(o);
            while ( i.more() ) {
                BSONElement e = i.next();
                BSONObj x = e.Obj();
                cout << setw( x["width"].numberInt() ) << e.fieldName() << ' ';
            }
            cout << endl;
        }

        static void printData( const BSONObj& o , const BSONObj& headers ) {

            BSONObjIterator i(headers);
            while ( i.more() ) {
                BSONElement e = i.next();
                BSONObj h = e.Obj();
                int w = h["width"].numberInt();

                BSONElement data;
                {
                    BSONElement temp = o[e.fieldName()];
                    if ( temp.isABSONObj() )
                        data = temp.Obj()["data"];
                }

                if ( data.type() == String )
                    cout << setw(w) << data.String();
                else if ( data.type() == NumberDouble )
                    cout << setw(w) << setprecision(3) << data.number();
                else if ( data.type() == NumberInt )
                    cout << setw(w) << data.numberInt();
                else if ( data.eoo() )
                    cout << setw(w) << "";
                else
                    cout << setw(w) << "???";

                cout << ' ';
            }
            cout << endl;
        }

        int runNormal() {
            int rowNum = 0;

            BSONObj prev = stats();
            if ( prev.isEmpty() )
                return -1;

            int maxLockedDbWidth = 0;
            bool warned = false;

            while (mongoStatGlobalParams.rowCount == 0 ||
                   rowNum < mongoStatGlobalParams.rowCount) {
                sleepsecs((int)ceil(_statUtil.getSeconds()));
                BSONObj now;
                try {
                    now = stats();
                }
                catch ( std::exception& e ) {
                    toolError() << "can't get data: " << e.what() << std::endl;
                    continue;
                }

                if ( now.isEmpty() )
                    return -2;

                try {

                    if ( !warned && now["storageEngine"].type() ) {
                       toolError() << "warning: detected a 3.0 mongod, some columns not applicable" << endl;
                       warned = true;
                    }

                    BSONObj out = _statUtil.doRow( prev , now );

                    // adjust width up as longer 'locked db' values appear
                    setMaxLockedDbWidth( &out, &maxLockedDbWidth ); 
                    if (mongoStatGlobalParams.showHeaders && rowNum % 10 == 0) {
                        printHeaders( out );
                    }

                    printData( out , out );

                }
                catch ( AssertionException& e ) {
                    toolError() << "\nerror: " << e.what() << "\n"
                              << now
                              << std::endl;
                }

                prev = now;
                rowNum++;
            }
            return 0;
        }

        /* Get the size of the 'locked db' field from a row of stats. If 
         * smaller than the current column width, set to the max.  If
         * greater, set the maxWidth to that value.
         */
        void setMaxLockedDbWidth( const BSONObj* o, int* maxWidth ) {
            BSONElement e = o->getField("locked db");
            if ( e.isABSONObj() ) {
                BSONObj x = e.Obj();
                if ( x["width"].numberInt() < *maxWidth ) {
                    BSONElementManipulator manip( x["width"] );
                    manip.setNumber( *maxWidth );
                }
                else {
                    *maxWidth = x["width"].numberInt();
                }
            }
        }

        struct ServerState {
            ServerState() : lock( "Stat::ServerState" ) {}
            string host;
            scoped_ptr<boost::thread> thr;

            mongo::mutex lock;

            BSONObj prev;
            BSONObj now;
            time_t lastUpdate;
            vector<BSONObj> shards;

            string error;
            bool mongos;

            BSONObj authParams;
        };

        static void serverThread( shared_ptr<ServerState> state , int sleepTime) {
            try {
                bool warned = false;
                DBClientConnection conn( true );
                conn._logLevel = logger::LogSeverity::Debug(1);
                string errmsg;
                if ( ! conn.connect( state->host , errmsg ) )
                    state->error = errmsg;
                long long cycleNumber = 0;

                if (! (state->authParams["user"].str().empty()) )
                    conn.auth(state->authParams);

                while ( ++cycleNumber ) {
                    try {
                        BSONObj out;
                        if ( conn.simpleCommand( "admin" , &out , "serverStatus" ) ) {
                            scoped_lock lk( state->lock );
                            state->error = "";
                            state->lastUpdate = time(0);
                            state->prev = state->now;
                            state->now = out.getOwned();
                        }
                        else {
                            str::stream errorStream;
                            errorStream << "serverStatus failed";
                            BSONElement errorField = out["errmsg"];
                            if (errorField.type() == String)
                                errorStream << ": " << errorField.str();
                            scoped_lock lk( state->lock );
                            state->error = errorStream;
                            state->lastUpdate = time(0);
                        }
                        if ( !warned && out["storageEngine"].type() ) {
                           toolError() << "warning: detected a 3.0 mongod, some columns not applicable" << endl;
                           warned = true;
                        }

                        if ( out["shardCursorType"].type() == Object ||
                             out["process"].str() == "mongos" ) {
                            state->mongos = true;
                            if ( cycleNumber % 10 == 1 ) {
                                auto_ptr<DBClientCursor> c = conn.query( ShardType::ConfigNS , BSONObj() );
                                vector<BSONObj> shards;
                                while ( c->more() ) {
                                    shards.push_back( c->nextSafe().getOwned() );
                                }
                                scoped_lock lk( state->lock );
                                state->shards = shards;
                            }
                        }
                    }
                    catch ( std::exception& e ) {
                        scoped_lock lk( state->lock );
                        state->error = e.what();
                    }

                    sleepsecs( sleepTime );
                }


            }
            catch ( std::exception& e ) {
                toolError() << "serverThread (" << state->host << ") fatal error : " << e.what()
                          << std::endl;
            }
            catch ( ... ) {
                toolError() << "serverThread (" << state->host << ") fatal error" << std::endl;
            }
        }

        typedef map<string,shared_ptr<ServerState> >  StateMap;

        bool _add( StateMap& threads , string host ) {
            shared_ptr<ServerState>& state = threads[host];
            if ( state )
                return false;

            state.reset( new ServerState() );
            state->host = host;
            /* For each new thread, pass in a thread state object and the delta between samples */
            state->thr.reset( new boost::thread( boost::bind( serverThread,
                                                              state,
                                                              (int)ceil(_statUtil.getSeconds()) ) ) );
            state->authParams = BSON(saslCommandUserFieldName << toolGlobalParams.username
                                  << saslCommandPasswordFieldName << toolGlobalParams.password
                                  << saslCommandUserDBFieldName << getAuthenticationDatabase()
                                  << saslCommandMechanismFieldName
                                  << toolGlobalParams.authenticationMechanism);
            return true;
        }

        /**
         * @param hosts [ "a.foo.com" , "b.foo.com" ]
         */
        bool _addAll( StateMap& threads , const BSONObj& hosts ) {
            BSONObjIterator i( hosts );
            bool added = false;
            while ( i.more() ) {
                bool me = _add( threads , i.next().String() );
                added = added || me;
            }
            return added;
        }

        bool _discover( StateMap& threads , const string& host , const shared_ptr<ServerState>& ss ) {

            BSONObj info = ss->now;

            bool found = false;

            if ( info["repl"].isABSONObj() ) {
                BSONObj x = info["repl"].Obj();
                if ( x["hosts"].isABSONObj() )
                    if ( _addAll( threads , x["hosts"].Obj() ) )
                        found = true;
                if ( x["passives"].isABSONObj() )
                    if ( _addAll( threads , x["passives"].Obj() ) )
                        found = true;
            }

            if ( ss->mongos ) {
                for ( unsigned i=0; i<ss->shards.size(); i++ ) {
                    BSONObj x = ss->shards[i];

                    string errmsg;
                    ConnectionString cs = ConnectionString::parse( x["host"].String() , errmsg );
                    if ( errmsg.size() ) {
                        toolError() << errmsg << std::endl;
                        continue;
                    }

                    vector<HostAndPort> v = cs.getServers();
                    for ( unsigned i=0; i<v.size(); i++ ) {
                        if ( _add( threads , v[i].toString() ) )
                            found = true;
                    }
                }
            }

            return found;
        }

        int runMany() {
            StateMap threads;

            {
                string orig = "localhost";
                bool showPorts = false;
                if (toolGlobalParams.hostSet) {
                    orig = toolGlobalParams.host;
                }

                if (orig.find(":") != string::npos || toolGlobalParams.portSet)
                    showPorts = true;

                StringSplitter ss( orig.c_str() , "," );
                while ( ss.more() ) {
                    string host = ss.next();
                    if ( showPorts && host.find( ":" ) == string::npos) {
                        // port supplied, but not for this host.  use default.
                        StringBuilder sb;
                        if (toolGlobalParams.portSet) {
                            sb << host << ":" << toolGlobalParams.port;
                        }
                        else {
                            sb << host << ":27017";
                        }
                        host = sb.str();
                    }
                    _add( threads , host );
                }
            }

            sleepsecs(1);

            int row = 0;
            int maxLockedDbWidth = 0;

            while (mongoStatGlobalParams.rowCount == 0 || row < mongoStatGlobalParams.rowCount) {
                sleepsecs( (int)ceil(_statUtil.getSeconds()) );

                // collect data
                vector<Row> rows;
                for ( map<string,shared_ptr<ServerState> >::iterator i=threads.begin(); i!=threads.end(); ++i ) {
                    scoped_lock lk( i->second->lock );

                    if ( i->second->error.size() ) {
                        rows.push_back( Row( i->first , i->second->error ) );
                    }
                    else if ( i->second->prev.isEmpty() || i->second->now.isEmpty() ) {
                        rows.push_back( Row( i->first ) );
                    }
                    else {
                        BSONObj out = _statUtil.doRow( i->second->prev , i->second->now );
                        rows.push_back( Row( i->first , out ) );
                    }

                    if (mongoStatGlobalParams.discover && ! i->second->now.isEmpty()) {
                        if ( _discover( threads , i->first , i->second ) )
                            break;
                    }
                }

                // compute some stats
                unsigned longestHost = 0;
                BSONObj biggest;
                for ( unsigned i=0; i<rows.size(); i++ ) {
                    if ( rows[i].host.size() > longestHost )
                        longestHost = rows[i].host.size();
                    if ( rows[i].data.nFields() > biggest.nFields() )
                        biggest = rows[i].data;

                    // adjust width up as longer 'locked db' values appear
                    setMaxLockedDbWidth( &rows[i].data, &maxLockedDbWidth ); 
                }

                {
                    // check for any headers not in biggest

                    // TODO: we put any new headers at end,
                    //       ideally we would interleave

                    set<string> seen;

                    BSONObjBuilder b;

                    {
                        // iterate biggest
                        BSONObjIterator i( biggest );
                        while ( i.more() ) {
                            BSONElement e = i.next();
                            seen.insert( e.fieldName() );
                            b.append( e );
                        }
                    }

                    // now do the rest
                    for ( unsigned j=0; j<rows.size(); j++ ) {
                        BSONObjIterator i( rows[j].data );
                        while ( i.more() ) {
                            BSONElement e = i.next();
                            if ( seen.count( e.fieldName() ) )
                                continue;
                            seen.insert( e.fieldName() );
                            b.append( e );
                        }

                    }

                    biggest = b.obj();

                }

                // display data

                cout << endl;

                //    header
                if (row++ % 5 == 0 && mongoStatGlobalParams.showHeaders && !biggest.isEmpty()) {
                    cout << setw( longestHost ) << "" << "\t";
                    printHeaders( biggest );
                }

                //    rows
                for ( unsigned i=0; i<rows.size(); i++ ) {
                    cout << setw( longestHost ) << rows[i].host << "\t";
                    if ( rows[i].err.size() )
                        cout << rows[i].err << endl;
                    else if ( rows[i].data.isEmpty() )
                        cout << "no data" << endl;
                    else
                        printData( rows[i].data , biggest );
                }

            }

            return 0;
        }

        StatUtil _statUtil;

        struct Row {
            Row( string h , string e ) {
                host = h;
                err = e;
            }

            Row( string h ) {
                host = h;
            }

            Row( string h , BSONObj d ) {
                host = h;
                data = d;
            }
            string host;
            string err;
            BSONObj data;
        };
    };
    REGISTER_MONGO_TOOL(Stat);
}
