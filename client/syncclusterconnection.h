// @file syncclusterconnection.h

/*
 *    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */


#include "../pch.h"
#include "dbclient.h"
#include "redef_macros.h"

namespace mongo {

    /**
     * This is a connection to a cluster of servers that operate as one
     * for super high durability.
     * 
     * Write operations are two-phase.  First, all nodes are asked to fsync. If successful
     * everywhere, the write is sent everywhere and then followed by an fsync.  There is no 
     * rollback if a problem occurs during the second phase.  Naturally, with all these fsyncs, 
     * these operations will be quite slow -- use sparingly.
     * 
     * Read operations are sent to a single random node.
     * 
     * The class checks if a command is read or write style, and sends to a single 
     * node if a read lock command and to all in two phases with a write style command.
     */
    class SyncClusterConnection : public DBClientBase {
    public:
        /**
         * @param commaSeparated should be 3 hosts comma separated
         */
        SyncClusterConnection( const list<HostAndPort> & );
        SyncClusterConnection( string commaSeparated );
        SyncClusterConnection( string a , string b , string c );
        ~SyncClusterConnection();
        
        /**
         * @return true if all servers are up and ready for writes
         */
        bool prepare( string& errmsg );

        /**
         * runs fsync on all servers
         */
        bool fsync( string& errmsg );

        // --- from DBClientInterface

        virtual BSONObj findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn, int queryOptions);

        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn, int nToSkip,
                                               const BSONObj *fieldsToReturn, int queryOptions, int batchSize );

        virtual auto_ptr<DBClientCursor> getMore( const string &ns, long long cursorId, int nToReturn, int options );
        
        virtual void insert( const string &ns, BSONObj obj );
        
        virtual void insert( const string &ns, const vector< BSONObj >& v );

        virtual void remove( const string &ns , Query query, bool justOne );

        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert , bool multi );

        virtual bool call( Message &toSend, Message &response, bool assertOk );
        virtual void say( Message &toSend );
        virtual void sayPiggyBack( Message &toSend );

        virtual void killCursor( long long cursorID );
        
        virtual string getServerAddress() const { return _address; }
        virtual bool isFailed() const { return false; }
        virtual string toString() { return _toString(); }

		virtual BSONObj getLastErrorDetailed();

        virtual bool callRead( Message& toSend , Message& response );

        virtual ConnectionString::ConnectionType type() const { return ConnectionString::SYNC; }  

        virtual bool isMember( const DBConnector * conn ) const;

    private:
        SyncClusterConnection( SyncClusterConnection& prev );
        string _toString() const;        
        bool _commandOnActive(const string &dbname, const BSONObj& cmd, BSONObj &info, int options=0);
        auto_ptr<DBClientCursor> _queryOnActive(const string &ns, Query query, int nToReturn, int nToSkip,
                                                const BSONObj *fieldsToReturn, int queryOptions, int batchSize );
        int _lockType( const string& name );
        void _checkLast();
        void _connect( string host );

        string _address;
        vector<string> _connAddresses;
        vector<DBClientConnection*> _conns;
        map<string,int> _lockTypes;
        mongo::mutex _mutex;
        
        vector<BSONObj> _lastErrors;
    };
    
    class UpdateNotTheSame : public UserException {
    public:
        UpdateNotTheSame( int code , const string& msg , const vector<string>& addrs , const vector<BSONObj>& lastErrors )
            : UserException( code , msg ) , _addrs( addrs ) , _lastErrors( lastErrors ){
            assert( _addrs.size() == _lastErrors.size() );
        }
        
        virtual ~UpdateNotTheSame() throw() {
        }

        unsigned size() const {
            return _addrs.size();
        }

        pair<string,BSONObj> operator[](unsigned i) const {
            return make_pair( _addrs[i] , _lastErrors[i] );
        }

    private:

        vector<string> _addrs;
        vector<BSONObj> _lastErrors;
    };
    
};

#include "undef_macros.h"
