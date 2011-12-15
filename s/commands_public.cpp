// s/commands_public.cpp


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
#include "../util/net/message.h"
#include "../db/dbmessage.h"
#include "../client/connpool.h"
#include "../client/parallel.h"
#include "../db/commands.h"
#include "../db/queryutil.h"
#include "../scripting/engine.h"

#include "config.h"
#include "chunk.h"
#include "strategy.h"
#include "grid.h"
#include "mr_shard.h"
#include "client.h"

namespace mongo {

    bool setParmsMongodSpecific(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl )
    { 
        return true;
    }

    namespace dbgrid_pub_cmds {

        class PublicGridCommand : public Command {
        public:
            PublicGridCommand( const char* n, const char* oldname=NULL ) : Command( n, false, oldname ) {
            }
            virtual bool slaveOk() const {
                return true;
            }
            virtual bool adminOnly() const {
                return false;
            }

            // Override if passthrough should also send query options
            // Safer as off by default, can slowly enable as we add more tests
            virtual bool passOptions() const { return false; }

            // all grid commands are designed not to lock
            virtual LockType locktype() const { return NONE; }

        protected:

            bool passthrough( DBConfigPtr conf, const BSONObj& cmdObj , BSONObjBuilder& result ) {
                return _passthrough(conf->getName(), conf, cmdObj, 0, result);
            }
            bool adminPassthrough( DBConfigPtr conf, const BSONObj& cmdObj , BSONObjBuilder& result ) {
                return _passthrough("admin", conf, cmdObj, 0, result);
            }

            bool passthrough( DBConfigPtr conf, const BSONObj& cmdObj , int options, BSONObjBuilder& result ) {
                return _passthrough(conf->getName(), conf, cmdObj, options, result);
            }
            bool adminPassthrough( DBConfigPtr conf, const BSONObj& cmdObj , int options, BSONObjBuilder& result ) {
                return _passthrough("admin", conf, cmdObj, options, result);
            }

        private:
            bool _passthrough(const string& db,  DBConfigPtr conf, const BSONObj& cmdObj , int options , BSONObjBuilder& result ) {
                ShardConnection conn( conf->getPrimary() , "" );
                BSONObj res;
                bool ok = conn->runCommand( db , cmdObj , res , passOptions() ? options : 0 );
                if ( ! ok && res["code"].numberInt() == StaleConfigInContextCode ) {
                    conn.done();
                    throw StaleConfigException("foo","command failed because of stale config");
                }
                result.appendElements( res );
                conn.done();
                return ok;
            }
        };

        class RunOnAllShardsCommand : public Command {
        public:
            RunOnAllShardsCommand(const char* n, const char* oldname=NULL) : Command(n, false, oldname) {}

            virtual bool slaveOk() const { return true; }
            virtual bool adminOnly() const { return false; }

            // all grid commands are designed not to lock
            virtual LockType locktype() const { return NONE; }


            // default impl uses all shards for DB
            virtual void getShards(const string& dbName , BSONObj& cmdObj, set<Shard>& shards) {
                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                conf->getAllShards(shards);
            }

            virtual void aggregateResults(const vector<BSONObj>& results, BSONObjBuilder& output) {}

            // don't override
            virtual bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& output, bool) {
                LOG(1) << "RunOnAllShardsCommand db: " << dbName << " cmd:" << cmdObj << endl;
                set<Shard> shards;
                getShards(dbName, cmdObj, shards);

                list< shared_ptr<Future::CommandResult> > futures;
                for ( set<Shard>::const_iterator i=shards.begin(), end=shards.end() ; i != end ; i++ ) {
                    futures.push_back( Future::spawnCommand( i->getConnString() , dbName , cmdObj, 0 ) );
                }

                vector<BSONObj> results;
                BSONObjBuilder subobj (output.subobjStart("raw"));
                BSONObjBuilder errors;
                for ( list< shared_ptr<Future::CommandResult> >::iterator i=futures.begin(); i!=futures.end(); i++ ) {
                    shared_ptr<Future::CommandResult> res = *i;
                    if ( ! res->join() ) {
                        errors.appendAs(res->result()["errmsg"], res->getServer());
                    }
                    results.push_back( res->result() );
                    subobj.append( res->getServer() , res->result() );
                }

                subobj.done();

                BSONObj errobj = errors.done();
                if (! errobj.isEmpty()) {
                    errmsg = errobj.toString(false, true);
                    return false;
                }

                aggregateResults(results, output);
                return true;
            }

        };

        class AllShardsCollectionCommand : public RunOnAllShardsCommand {
        public:
            AllShardsCollectionCommand(const char* n, const char* oldname=NULL) : RunOnAllShardsCommand(n, oldname) {}

            virtual void getShards(const string& dbName , BSONObj& cmdObj, set<Shard>& shards) {
                string fullns = dbName + '.' + cmdObj.firstElement().valuestrsafe();

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    shards.insert(conf->getShard(fullns));
                }
                else {
                    conf->getChunkManager(fullns)->getAllShards(shards);
                }
            }
        };


        class NotAllowedOnShardedCollectionCmd : public PublicGridCommand {
        public:
            NotAllowedOnShardedCollectionCmd( const char * n ) : PublicGridCommand( n ) {}

            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) = 0;

            virtual bool run(const string& dbName , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool) {
                string fullns = getFullNS( dbName , cmdObj );

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , options, result );
                }
                errmsg = "can't do command: " + name + " on sharded collection";
                return false;
            }
        };

        // ----

        class DropIndexesCmd : public AllShardsCollectionCommand {
        public:
            DropIndexesCmd() :  AllShardsCollectionCommand("dropIndexes", "deleteIndexes") {}
        } dropIndexesCmd;

        class ReIndexCmd : public AllShardsCollectionCommand {
        public:
            ReIndexCmd() :  AllShardsCollectionCommand("reIndex") {}
        } reIndexCmd;

        class ProfileCmd : public PublicGridCommand {
        public:
            ProfileCmd() :  PublicGridCommand("profile") {}
            virtual bool run(const string& dbName , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg = "profile currently not supported via mongos";
                return false;
            }
        } profileCmd;
        

        class ValidateCmd : public AllShardsCollectionCommand {
        public:
            ValidateCmd() :  AllShardsCollectionCommand("validate") {}
            virtual void aggregateResults(const vector<BSONObj>& results, BSONObjBuilder& output) {
                for (vector<BSONObj>::const_iterator it(results.begin()), end(results.end()); it!=end; it++){
                    const BSONObj& result = *it;
                    const BSONElement valid = result["valid"];
                    if (!valid.eoo()){
                        if (!valid.trueValue()) {
                            output.appendBool("valid", false);
                            return;
                        }
                    }
                    else {
                        // Support pre-1.9.0 output with everything in a big string
                        const char* s = result["result"].valuestrsafe();
                        if (strstr(s, "exception") ||  strstr(s, "corrupt")){
                            output.appendBool("valid", false);
                            return;
                        }
                    }
                }

                output.appendBool("valid", true);
            }
        } validateCmd;

        class RepairDatabaseCmd : public RunOnAllShardsCommand {
        public:
            RepairDatabaseCmd() :  RunOnAllShardsCommand("repairDatabase") {}
        } repairDatabaseCmd;

        class DBStatsCmd : public RunOnAllShardsCommand {
        public:
            DBStatsCmd() :  RunOnAllShardsCommand("dbStats", "dbstats") {}

            virtual void aggregateResults(const vector<BSONObj>& results, BSONObjBuilder& output) {
                long long objects = 0;
                long long dataSize = 0;
                long long storageSize = 0;
                long long numExtents = 0;
                long long indexes = 0;
                long long indexSize = 0;
                long long fileSize = 0;

                for (vector<BSONObj>::const_iterator it(results.begin()), end(results.end()); it != end; ++it) {
                    const BSONObj& b = *it;
                    objects     += b["objects"].numberLong();
                    dataSize    += b["dataSize"].numberLong();
                    storageSize += b["storageSize"].numberLong();
                    numExtents  += b["numExtents"].numberLong();
                    indexes     += b["indexes"].numberLong();
                    indexSize   += b["indexSize"].numberLong();
                    fileSize    += b["fileSize"].numberLong();
                }

                //result.appendNumber( "collections" , ncollections ); //TODO: need to find a good way to get this
                output.appendNumber( "objects" , objects );
                output.append      ( "avgObjSize" , double(dataSize) / double(objects) );
                output.appendNumber( "dataSize" , dataSize );
                output.appendNumber( "storageSize" , storageSize);
                output.appendNumber( "numExtents" , numExtents );
                output.appendNumber( "indexes" , indexes );
                output.appendNumber( "indexSize" , indexSize );
                output.appendNumber( "fileSize" , fileSize );
            }
        } DBStatsCmdObj;

        class DropCmd : public PublicGridCommand {
        public:
            DropCmd() : PublicGridCommand( "drop" ) {}
            bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                log() << "DROP: " << fullns << endl;

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result );
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 10418 ,  "how could chunk manager be null!" , cm );

                cm->drop( cm );
                uassert( 13512 , "drop collection attempted on non-sharded collection" , conf->removeSharding( fullns ) );

                return 1;
            }
        } dropCmd;

        class DropDBCmd : public PublicGridCommand {
        public:
            DropDBCmd() : PublicGridCommand( "dropDatabase" ) {}
            bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {

                BSONElement e = cmdObj.firstElement();

                if ( ! e.isNumber() || e.number() != 1 ) {
                    errmsg = "invalid params";
                    return 0;
                }

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                log() << "DROP DATABASE: " << dbName << endl;

                if ( ! conf ) {
                    result.append( "info" , "database didn't exist" );
                    return true;
                }

                if ( ! conf->dropDatabase( errmsg ) )
                    return false;

                result.append( "dropped" , dbName );
                return true;
            }
        } dropDBCmd;

        class RenameCollectionCmd : public PublicGridCommand {
        public:
            RenameCollectionCmd() : PublicGridCommand( "renameCollection" ) {}
            bool run(const string& dbName, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string fullnsFrom = cmdObj.firstElement().valuestrsafe();
                string dbNameFrom = nsToDatabase( fullnsFrom.c_str() );
                DBConfigPtr confFrom = grid.getDBConfig( dbNameFrom , false );

                string fullnsTo = cmdObj["to"].valuestrsafe();
                string dbNameTo = nsToDatabase( fullnsTo.c_str() );
                DBConfigPtr confTo = grid.getDBConfig( dbNameTo , false );

                uassert(13140, "Don't recognize source or target DB", confFrom && confTo);
                uassert(13138, "You can't rename a sharded collection", !confFrom->isSharded(fullnsFrom));
                uassert(13139, "You can't rename to a sharded collection", !confTo->isSharded(fullnsTo));

                const Shard& shardTo = confTo->getShard(fullnsTo);
                const Shard& shardFrom = confFrom->getShard(fullnsFrom);

                uassert(13137, "Source and destination collections must be on same shard", shardFrom == shardTo);

                return adminPassthrough( confFrom , cmdObj , result );
            }
        } renameCollectionCmd;

        class CopyDBCmd : public PublicGridCommand {
        public:
            CopyDBCmd() : PublicGridCommand( "copydb" ) {}
            bool run(const string& dbName, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string todb = cmdObj.getStringField("todb");
                uassert(13402, "need a todb argument", !todb.empty());

                DBConfigPtr confTo = grid.getDBConfig( todb );
                uassert(13398, "cant copy to sharded DB", !confTo->isShardingEnabled());

                string fromhost = cmdObj.getStringField("fromhost");
                if (!fromhost.empty()) {
                    return adminPassthrough( confTo , cmdObj , result );
                }
                else {
                    string fromdb = cmdObj.getStringField("fromdb");
                    uassert(13399, "need a fromdb argument", !fromdb.empty());

                    DBConfigPtr confFrom = grid.getDBConfig( fromdb , false );
                    uassert(13400, "don't know where source DB is", confFrom);
                    uassert(13401, "cant copy from sharded DB", !confFrom->isShardingEnabled());

                    BSONObjBuilder b;
                    BSONForEach(e, cmdObj) {
                        if (strcmp(e.fieldName(), "fromhost") != 0)
                            b.append(e);
                    }
                    b.append("fromhost", confFrom->getPrimary().getConnString());
                    BSONObj fixed = b.obj();

                    return adminPassthrough( confTo , fixed , result );
                }

            }
        } copyDBCmd;

        class CountCmd : public PublicGridCommand {
        public:
            CountCmd() : PublicGridCommand("count") { }
            virtual bool passOptions() const { return true; }
            bool run(const string& dbName, BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                BSONObj filter;
                if ( cmdObj["query"].isABSONObj() )
                    filter = cmdObj["query"].Obj();

                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    ShardConnection conn( conf->getPrimary() , fullns );

                    BSONObj temp;
                    bool ok = conn->runCommand( dbName , cmdObj , temp, options );
                    conn.done();

                    if ( ok ) {
                        result.append( temp["n"] );
                        return true;
                    }

                    if ( temp["code"].numberInt() != StaleConfigInContextCode ) {
                        errmsg = temp["errmsg"].String();
                        result.appendElements( temp );
                        return false;
                    }

                    // this collection got sharded
                    ChunkManagerPtr cm = conf->getChunkManagerIfExists( fullns , true );
                    if ( ! cm ) {
                        errmsg = "should be sharded now";
                        result.append( "root" , temp );
                        return false;
                    }
                }

                long long total = 0;
                map<string,long long> shardCounts;
                int numTries = 0;
                bool hadToBreak = false;

                ChunkManagerPtr cm = conf->getChunkManagerIfExists( fullns );
                while ( numTries < 5 ) {
                    numTries++;

                    // This all should eventually be replaced by new pcursor framework, but for now match query
                    // retry behavior manually
                    if( numTries >= 2 ) sleepsecs( numTries - 1 );

                    if ( ! cm ) {
                        // probably unsharded now
                        return run( dbName , cmdObj , options , errmsg , result, false );
                    }

                    set<Shard> shards;
                    cm->getShardsForQuery( shards , filter );
                    assert( shards.size() );

                    hadToBreak = false;

                    for (set<Shard>::iterator it=shards.begin(), end=shards.end(); it != end; ++it) {
                        ShardConnection conn(*it, fullns);
                        if ( conn.setVersion() ){
                            ChunkManagerPtr newCM = conf->getChunkManagerIfExists( fullns );
                            if( newCM->getVersion() != cm->getVersion() ){
                                cm = newCM;
                                total = 0;
                                shardCounts.clear();
                                conn.done();
                                hadToBreak = true;
                                break;
                            }
                        }

                        BSONObj temp;
                        bool ok = conn->runCommand( dbName , BSON( "count" << collection << "query" << filter ) , temp, options );
                        conn.done();

                        if ( ok ) {
                            long long mine = temp["n"].numberLong();
                            total += mine;
                            shardCounts[it->getName()] = mine;
                            continue;
                        }

                        if ( StaleConfigInContextCode == temp["code"].numberInt() ) {
                            // my version is old
                            total = 0;
                            shardCounts.clear();
                            cm = conf->getChunkManagerIfExists( fullns , true, numTries > 2 ); // Force reload on third attempt
                            hadToBreak = true;
                            break;
                        }

                        // command failed :(
                        errmsg = "failed on : " + it->getName();
                        result.append( "cause" , temp );
                        return false;
                    }
                    if ( ! hadToBreak )
                        break;
                }
                if (hadToBreak) {
                    errmsg = "Tried 5 times without success to get count for " + fullns + " from all shards";
                    return false;
                }

                total = applySkipLimit( total , cmdObj );
                result.appendNumber( "n" , total );
                BSONObjBuilder temp( result.subobjStart( "shards" ) );
                for ( map<string,long long>::iterator i=shardCounts.begin(); i!=shardCounts.end(); ++i )
                    temp.appendNumber( i->first , i->second );
                temp.done();
                return true;
            }
        } countCmd;

        class CollectionStats : public PublicGridCommand {
        public:
            CollectionStats() : PublicGridCommand("collStats", "collstats") { }
            bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    result.appendBool("sharded", false);
                    result.append( "primary" , conf->getPrimary().getName() );
                    return passthrough( conf , cmdObj , result);
                }
                result.appendBool("sharded", true);

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 12594 ,  "how could chunk manager be null!" , cm );

                set<Shard> servers;
                cm->getAllShards(servers);

                BSONObjBuilder shardStats;
                map<string,long long> counts;
                map<string,long long> indexSizes;
                /*
                long long count=0;
                long long size=0;
                long long storageSize=0;
                */
                int nindexes=0;
                bool warnedAboutIndexes = false;
                for ( set<Shard>::iterator i=servers.begin(); i!=servers.end(); i++ ) {
                    ScopedDbConnection conn( *i );
                    BSONObj res;
                    if ( ! conn->runCommand( dbName , cmdObj , res ) ) {
                        errmsg = "failed on shard: " + res.toString();
                        return false;
                    }
                    conn.done();
                    
                    BSONObjIterator j( res );
                    while ( j.more() ) {
                        BSONElement e = j.next();

                        if ( str::equals( e.fieldName() , "ns" ) || 
                             str::equals( e.fieldName() , "ok" ) || 
                             str::equals( e.fieldName() , "avgObjSize" ) ||
                             str::equals( e.fieldName() , "lastExtentSize" ) ||
                             str::equals( e.fieldName() , "paddingFactor" ) ) {
                            continue;
                        }
                        else if ( str::equals( e.fieldName() , "count" ) ||
                                  str::equals( e.fieldName() , "size" ) ||
                                  str::equals( e.fieldName() , "storageSize" ) ||
                                  str::equals( e.fieldName() , "numExtents" ) ||
                                  str::equals( e.fieldName() , "totalIndexSize" ) ) {
                            counts[e.fieldName()] += e.numberLong();
                        }
                        else if ( str::equals( e.fieldName() , "indexSizes" ) ) {
                            BSONObjIterator k( e.Obj() );
                            while ( k.more() ) {
                                BSONElement temp = k.next();
                                indexSizes[temp.fieldName()] += temp.numberLong();
                            }
                        }
                        else if ( str::equals( e.fieldName() , "flags" ) ) {
                            if ( ! result.hasField( e.fieldName() ) )
                                result.append( e );
                        }
                        else if ( str::equals( e.fieldName() , "nindexes" ) ) {
                            int myIndexes = e.numberInt();
                            
                            if ( nindexes == 0 ) {
                                nindexes = myIndexes;
                            }
                            else if ( nindexes == myIndexes ) {
                                // no-op
                            }
                            else {
                                // hopefully this means we're building an index
                                
                                if ( myIndexes > nindexes )
                                    nindexes = myIndexes;
                                
                                if ( ! warnedAboutIndexes ) {
                                    result.append( "warning" , "indexes don't all match - ok if ensureIndex is running" );
                                    warnedAboutIndexes = true;
                                }
                            }
                        }
                        else {
                            warning() << "mongos collstats doesn't know about: " << e.fieldName() << endl;
                        }
                        
                    }
                    shardStats.append(i->getName(), res);
                }

                result.append("ns", fullns);
                
                for ( map<string,long long>::iterator i=counts.begin(); i!=counts.end(); ++i )
                    result.appendNumber( i->first , i->second );
                
                {
                    BSONObjBuilder ib( result.subobjStart( "indexSizes" ) );
                    for ( map<string,long long>::iterator i=indexSizes.begin(); i!=indexSizes.end(); ++i )
                        ib.appendNumber( i->first , i->second );
                    ib.done();
                }

                if ( counts["count"] > 0 )
                    result.append("avgObjSize", (double)counts["size"] / (double)counts["count"] );
                else
                    result.append( "avgObjSize", 0.0 );
                
                result.append("nindexes", nindexes);

                result.append("nchunks", cm->numChunks());
                result.append("shards", shardStats.obj());

                return true;
            }
        } collectionStatsCmd;

        class FindAndModifyCmd : public PublicGridCommand {
        public:
            FindAndModifyCmd() : PublicGridCommand("findAndModify", "findandmodify") { }
            bool run(const string& dbName, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result);
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13002 ,  "shard internal error chunk manager should never be null" , cm );

                BSONObj filter = cmdObj.getObjectField("query");
                uassert(13343,  "query for sharded findAndModify must have shardkey", cm->hasShardKey(filter));

                //TODO with upsert consider tracking for splits

                ChunkPtr chunk = cm->findChunk(filter);
                ShardConnection conn( chunk->getShard() , fullns );
                BSONObj res;
                bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                conn.done();

                if (!ok && res.getIntField("code") == 9996) { // code for StaleConfigException
                    throw StaleConfigException(fullns, "FindAndModify"); // Command code traps this and re-runs
                }

                result.appendElements(res);
                return ok;
            }

        } findAndModifyCmd;

        class DataSizeCmd : public PublicGridCommand {
        public:
            DataSizeCmd() : PublicGridCommand("dataSize", "datasize") { }
            bool run(const string& dbName, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string fullns = cmdObj.firstElement().String();

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result);
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13407 ,  "how could chunk manager be null!" , cm );

                BSONObj min = cmdObj.getObjectField( "min" );
                BSONObj max = cmdObj.getObjectField( "max" );
                BSONObj keyPattern = cmdObj.getObjectField( "keyPattern" );

                uassert(13408,  "keyPattern must equal shard key", cm->getShardKey().key() == keyPattern);

                // yes these are doubles...
                double size = 0;
                double numObjects = 0;
                int millis = 0;

                set<Shard> shards;
                cm->getShardsForRange(shards, min, max);
                for ( set<Shard>::iterator i=shards.begin(), end=shards.end() ; i != end; ++i ) {
                    ScopedDbConnection conn( *i );
                    BSONObj res;
                    bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                    conn.done();

                    if ( ! ok ) {
                        result.appendElements( res );
                        return false;
                    }

                    size       += res["size"].number();
                    numObjects += res["numObjects"].number();
                    millis     += res["millis"].numberInt();

                }

                result.append( "size", size );
                result.append( "numObjects" , numObjects );
                result.append( "millis" , millis );
                return true;
            }

        } DataSizeCmd;

        class ConvertToCappedCmd : public NotAllowedOnShardedCollectionCmd  {
        public:
            ConvertToCappedCmd() : NotAllowedOnShardedCollectionCmd("convertToCapped") {}

            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) {
                return dbName + "." + cmdObj.firstElement().valuestrsafe();
            }

        } convertToCappedCmd;


        class GroupCmd : public NotAllowedOnShardedCollectionCmd  {
        public:
            GroupCmd() : NotAllowedOnShardedCollectionCmd("group") {}
            virtual bool passOptions() const { return true; }
            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) {
                return dbName + "." + cmdObj.firstElement().embeddedObjectUserCheck()["ns"].valuestrsafe();
            }

        } groupCmd;

        class DistinctCmd : public PublicGridCommand {
        public:
            DistinctCmd() : PublicGridCommand("distinct") {}
            virtual void help( stringstream &help ) const {
                help << "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
            }
            virtual bool passOptions() const { return true; }
            bool run(const string& dbName , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , options, result );
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 10420 ,  "how could chunk manager be null!" , cm );

                BSONObj query = getQuery(cmdObj);
                set<Shard> shards;
                cm->getShardsForQuery(shards, query);

                set<BSONObj,BSONObjCmp> all;
                int size = 32;

                for ( set<Shard>::iterator i=shards.begin(), end=shards.end() ; i != end; ++i ) {
                    ShardConnection conn( *i , fullns );
                    BSONObj res;
                    bool ok = conn->runCommand( conf->getName() , cmdObj , res, options );
                    conn.done();

                    if ( ! ok ) {
                        result.appendElements( res );
                        return false;
                    }

                    BSONObjIterator it( res["values"].embeddedObject() );
                    while ( it.more() ) {
                        BSONElement nxt = it.next();
                        BSONObjBuilder temp(32);
                        temp.appendAs( nxt , "" );
                        all.insert( temp.obj() );
                    }

                }

                BSONObjBuilder b( size );
                int n=0;
                for ( set<BSONObj,BSONObjCmp>::iterator i = all.begin() ; i != all.end(); i++ ) {
                    b.appendAs( i->firstElement() , b.numStr( n++ ) );
                }

                result.appendArray( "values" , b.obj() );
                return true;
            }
        } disinctCmd;

        class FileMD5Cmd : public PublicGridCommand {
        public:
            FileMD5Cmd() : PublicGridCommand("filemd5") {}
            virtual void help( stringstream &help ) const {
                help << " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
            }
            bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string fullns = dbName;
                fullns += ".";
                {
                    string root = cmdObj.getStringField( "root" );
                    if ( root.size() == 0 )
                        root = "fs";
                    fullns += root;
                }
                fullns += ".chunks";

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result );
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13091 , "how could chunk manager be null!" , cm );
                uassert( 13092 , "GridFS chunks collection can only be sharded on files_id", cm->getShardKey().key() == BSON("files_id" << 1));

                ChunkPtr chunk = cm->findChunk( BSON("files_id" << cmdObj.firstElement()) );

                ShardConnection conn( chunk->getShard() , fullns );
                BSONObj res;
                bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                conn.done();

                result.appendElements(res);
                return ok;
            }
        } fileMD5Cmd;

        class Geo2dFindNearCmd : public PublicGridCommand {
        public:
            Geo2dFindNearCmd() : PublicGridCommand( "geoNear" ) {}
            void help(stringstream& h) const { h << "http://www.mongodb.org/display/DOCS/Geospatial+Indexing#GeospatialIndexing-geoNearCommand"; }
            virtual bool passOptions() const { return true; }
            bool run(const string& dbName , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , options, result );
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13500 ,  "how could chunk manager be null!" , cm );

                BSONObj query = getQuery(cmdObj);
                set<Shard> shards;
                cm->getShardsForQuery(shards, query);

                int limit = 100;
                if (cmdObj["num"].isNumber())
                    limit = cmdObj["num"].numberInt();

                list< shared_ptr<Future::CommandResult> > futures;
                BSONArrayBuilder shardArray;
                for ( set<Shard>::const_iterator i=shards.begin(), end=shards.end() ; i != end ; i++ ) {
                    futures.push_back( Future::spawnCommand( i->getConnString() , dbName , cmdObj, options ) );
                    shardArray.append(i->getName());
                }

                multimap<double, BSONObj> results; // TODO: maybe use merge-sort instead
                string nearStr;
                double time = 0;
                double btreelocs = 0;
                double nscanned = 0;
                double objectsLoaded = 0;
                for ( list< shared_ptr<Future::CommandResult> >::iterator i=futures.begin(); i!=futures.end(); i++ ) {
                    shared_ptr<Future::CommandResult> res = *i;
                    if ( ! res->join() ) {
                        errmsg = res->result()["errmsg"].String();
                        return false;
                    }

                    nearStr = res->result()["near"].String();
                    time += res->result()["stats"]["time"].Number();
                    btreelocs += res->result()["stats"]["btreelocs"].Number();
                    nscanned += res->result()["stats"]["nscanned"].Number();
                    objectsLoaded += res->result()["stats"]["objectsLoaded"].Number();

                    BSONForEach(obj, res->result()["results"].embeddedObject()) {
                        results.insert(make_pair(obj["dis"].Number(), obj.embeddedObject().getOwned()));
                    }

                    // TODO: maybe shrink results if size() > limit
                }

                result.append("ns" , fullns);
                result.append("near", nearStr);

                int outCount = 0;
                double totalDistance = 0;
                double maxDistance = 0;
                {
                    BSONArrayBuilder sub (result.subarrayStart("results"));
                    for (multimap<double, BSONObj>::const_iterator it(results.begin()), end(results.end()); it!= end && outCount < limit; ++it, ++outCount) {
                        totalDistance += it->first;
                        maxDistance = it->first; // guaranteed to be highest so far

                        sub.append(it->second);
                    }
                    sub.done();
                }

                {
                    BSONObjBuilder sub (result.subobjStart("stats"));
                    sub.append("time", time);
                    sub.append("btreelocs", btreelocs);
                    sub.append("nscanned", nscanned);
                    sub.append("objectsLoaded", objectsLoaded);
                    sub.append("avgDistance", totalDistance / outCount);
                    sub.append("maxDistance", maxDistance);
                    sub.append("shards", shardArray.arr());
                    sub.done();
                }

                return true;
            }
        } geo2dFindNearCmd;

        class MRCmd : public PublicGridCommand {
        public:
            AtomicUInt JOB_NUMBER;

            MRCmd() : PublicGridCommand( "mapreduce" ) {}

            string getTmpName( const string& coll ) {
                stringstream ss;
                ss << "tmp.mrs." << coll << "_" << time(0) << "_" << JOB_NUMBER++;
                return ss.str();
            }

            BSONObj fixForShards( const BSONObj& orig , const string& output, BSONObj& customOut , string& badShardedField ) {
                BSONObjBuilder b;
                BSONObjIterator i( orig );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    string fn = e.fieldName();
                    if ( fn == "map" ||
                            fn == "mapreduce" ||
                            fn == "mapparams" ||
                            fn == "reduce" ||
                            fn == "query" ||
                            fn == "sort" ||
                            fn == "scope" ||
                            fn == "verbose" ) {
                        b.append( e );
                    }
                    else if ( fn == "out" ||
                              fn == "finalize" ) {
                        // we don't want to copy these
                    	if (fn == "out" && e.type() == Object) {
                    		// check if there is a custom output
                    		BSONObj out = e.embeddedObject();
//                    		if (out.hasField("db"))
                    	    customOut = out;
                    	}
                    }
                    else {
                        badShardedField = fn;
                        return BSONObj();
                    }
                }
                b.append( "out" , output );
                return b.obj();
            }

            bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                Timer t;

                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                const string shardedOutputCollection = getTmpName( collection );

                string badShardedField;
                BSONObj customOut;
                BSONObj shardedCommand = fixForShards( cmdObj , shardedOutputCollection, customOut , badShardedField );

                bool customOutDB = customOut.hasField( "db" );

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    if ( customOutDB ) {
                        errmsg = "can't use out 'db' with non-sharded db";
                        return false;
                    }
                    return passthrough( conf , cmdObj , result );
                }

                if ( badShardedField.size() ) {
                    errmsg = str::stream() << "unknown m/r field for sharding: " << badShardedField;
                    return false;
                }

                BSONObjBuilder timingBuilder;

                ChunkManagerPtr cm = conf->getChunkManager( fullns );

                BSONObj q;
                if ( cmdObj["query"].type() == Object ) {
                    q = cmdObj["query"].embeddedObjectUserCheck();
                }

                set<Shard> shards;
                cm->getShardsForQuery( shards , q );


                BSONObjBuilder finalCmd;
                finalCmd.append( "mapreduce.shardedfinish" , cmdObj );
                finalCmd.append( "shardedOutputCollection" , shardedOutputCollection );
                
                
                set<ServerAndQuery> servers;
                BSONObj shardCounts;
                BSONObj aggCounts;
                map<string,long long> countsMap;
                {
                    // we need to use our connections to the shard
                    // so filtering is done correctly for un-owned docs
                    // so we allocate them in our thread
                    // and hand off
                    // Note: why not use pooled connections? This has been reported to create too many connections
                    vector< shared_ptr<ShardConnection> > shardConns;
                    list< shared_ptr<Future::CommandResult> > futures;
                    
                    for ( set<Shard>::iterator i=shards.begin(), end=shards.end() ; i != end ; i++ ) {
                        shared_ptr<ShardConnection> temp( new ShardConnection( i->getConnString() , fullns ) );
                        assert( temp->get() );
                        futures.push_back( Future::spawnCommand( i->getConnString() , dbName , shardedCommand , 0 , temp->get() ) );
                        shardConns.push_back( temp );
                    }
                    
                    bool failed = false;

                    // now wait for the result of all shards
                    BSONObjBuilder shardResultsB;
                    BSONObjBuilder shardCountsB;
                    BSONObjBuilder aggCountsB;
                    for ( list< shared_ptr<Future::CommandResult> >::iterator i=futures.begin(); i!=futures.end(); i++ ) {
                        shared_ptr<Future::CommandResult> res = *i;
                        if ( ! res->join() ) {
                            error() << "sharded m/r failed on shard: " << res->getServer() << " error: " << res->result() << endl;
                            result.append( "cause" , res->result() );
                            errmsg = "mongod mr failed: ";
                            errmsg += res->result().toString();
                            failed = true;
                            continue;
                        }
                        BSONObj result = res->result();
                        shardResultsB.append( res->getServer() , result );
                        BSONObj counts = result["counts"].embeddedObjectUserCheck();
                        shardCountsB.append( res->getServer() , counts );
                        servers.insert(res->getServer());

                        // add up the counts for each shard
                        // some of them will be fixed later like output and reduce
                        BSONObjIterator j( counts );
                        while ( j.more() ) {
                            BSONElement temp = j.next();
                            countsMap[temp.fieldName()] += temp.numberLong();
                        }
                    }

                    for ( unsigned i=0; i<shardConns.size(); i++ )
                        shardConns[i]->done();

                    if ( failed )
                        return 0;

                    finalCmd.append( "shards" , shardResultsB.obj() );
                    shardCounts = shardCountsB.obj();
                    finalCmd.append( "shardCounts" , shardCounts );
                    timingBuilder.append( "shards" , t.millis() );

                    for ( map<string,long long>::iterator i=countsMap.begin(); i!=countsMap.end(); i++ ) {
                        aggCountsB.append( i->first , i->second );
                    }
                    aggCounts = aggCountsB.obj();
                    finalCmd.append( "counts" , aggCounts );
                }

                Timer t2;
                BSONObj finalResult;
                bool ok = false;
                string outdb = dbName;
                if (customOutDB) {
                    BSONElement elmt = customOut.getField("db");
                    outdb = elmt.valuestrsafe();
                }

                if (!customOut.getBoolField("sharded")) {
                    // non-sharded, use the MRFinish command on target server
                    // This will save some data transfer

                    // by default the target database is same as input
                    Shard outServer = conf->getPrimary();
                    string outns = fullns;
                    if ( customOutDB ) {
                        // have to figure out shard for the output DB
                        DBConfigPtr conf2 = grid.getDBConfig( outdb , true );
                        outServer = conf2->getPrimary();
                        outns = outdb + "." + collection;
                    }
                    log() << "customOut: " << customOut << " outServer: " << outServer << endl;

                    ShardConnection conn( outServer , outns );
                    ok = conn->runCommand( dbName , finalCmd.obj() , finalResult );
                    conn.done();
                } else {
                    // grab records from each shard and insert back in correct shard in "temp" collection
                    // we do the final reduce in mongos since records are ordered and already reduced on each shard
//                    string shardedIncLong = str::stream() << outdb << ".tmp.mr." << collection << "_" << "shardedTemp" << "_" << time(0) << "_" << JOB_NUMBER++;

                    mr_shard::Config config( dbName , cmdObj );
                    mr_shard::State state(config);
                    LOG(1) << "mr sharded output ns: " << config.ns << endl;

                    if (config.outType == mr_shard::Config::INMEMORY) {
                        errmsg = "This Map Reduce mode is not supported with sharded output";
                        return false;
                    }

                    if (!config.outDB.empty()) {
                        BSONObjBuilder loc;
                        if ( !config.outDB.empty())
                            loc.append( "db" , config.outDB );
                        loc.append( "collection" , config.finalShort );
                        result.append("result", loc.obj());
                    }
                    else {
                        if ( !config.finalShort.empty() )
                            result.append( "result" , config.finalShort );
                    }

                    string outns = config.finalLong;
                    string tempns;

                    // result will be inserted into a temp collection to post process
                    const string postProcessCollection = getTmpName( collection );
                    finalCmd.append("postProcessCollection", postProcessCollection);
                    tempns = dbName + "." + postProcessCollection;

//                    if (config.outType == mr_shard::Config::REPLACE) {
//                        // drop previous collection
//                        BSONObj dropColCmd = BSON("drop" << config.finalShort);
//                        BSONObjBuilder dropColResult(32);
//                        string outdbCmd = outdb + ".$cmd";
//                        bool res = Command::runAgainstRegistered(outdbCmd.c_str(), dropColCmd, dropColResult);
//                        if (!res) {
//                            errmsg = str::stream() << "Could not drop sharded output collection " << outns << ": " << dropColResult.obj().toString();
//                            return false;
//                        }
//                    }

                    BSONObj sortKey = BSON( "_id" << 1 );
                    if (!conf->isSharded(outns)) {
                        // create the sharded collection

                        BSONObj shardColCmd = BSON("shardCollection" << outns << "key" << sortKey);
                        BSONObjBuilder shardColResult(32);
                        bool res = Command::runAgainstRegistered("admin.$cmd", shardColCmd, shardColResult);
                        if (!res) {
                            errmsg = str::stream() << "Could not create sharded output collection " << outns << ": " << shardColResult.obj().toString();
                            return false;
                        }
                    }

                    ParallelSortClusteredCursor cursor( servers , dbName + "." + shardedOutputCollection ,
                                                        Query().sort( sortKey ) );
                    cursor.init();
                    state.init();

                    mr_shard::BSONList values;
                    Strategy* s = SHARDED;
                    long long finalCount = 0;
                    int currentSize = 0;
                    while ( cursor.more() || !values.empty() ) {
                        BSONObj t;
                        if ( cursor.more() ) {
                            t = cursor.next().getOwned();

                            if ( values.size() == 0 || t.woSortOrder( *(values.begin()) , sortKey ) == 0 ) {
                                values.push_back( t );
                                currentSize += t.objsize();

                                // check size and potentially reduce
                                if (currentSize > config.maxInMemSize && values.size() > config.reduceTriggerRatio) {
                                    BSONObj reduced = config.reducer->finalReduce(values, 0);
                                    values.clear();
                                    values.push_back( reduced );
                                    currentSize = reduced.objsize();
                                }
                                continue;
                            }
                        }

                        BSONObj final = config.reducer->finalReduce(values, config.finalizer.get());
                        if (config.outType == mr_shard::Config::MERGE) {
                            BSONObj id = final["_id"].wrap();
                            s->updateSharded(conf, outns.c_str(), id, final, UpdateOption_Upsert, true);
                        } else {
                            // insert into temp collection, but using final collection's shard chunks
                            s->insertSharded(conf, tempns.c_str(), final, 0, true, outns.c_str());
                        }
                        ++finalCount;
                        values.clear();
                        if (!t.isEmpty()) {
                            values.push_back( t );
                            currentSize = t.objsize();
                        }
                    }

                    if (config.outType == mr_shard::Config::REDUCE || config.outType == mr_shard::Config::REPLACE) {
                        // results were written to temp collection, need post processing
                        vector< shared_ptr<ShardConnection> > shardConns;
                        list< shared_ptr<Future::CommandResult> > futures;
                        BSONObj finalCmdObj = finalCmd.obj();
                        for ( set<Shard>::iterator i=shards.begin(), end=shards.end() ; i != end ; i++ ) {
                            shared_ptr<ShardConnection> temp( new ShardConnection( i->getConnString() , outns ) );
                            futures.push_back( Future::spawnCommand( i->getConnString() , dbName , finalCmdObj , 0 , temp->get() ) );
                            shardConns.push_back( temp );
                        }

                        // now wait for the result of all shards
                        bool failed = false;
                        for ( list< shared_ptr<Future::CommandResult> >::iterator i=futures.begin(); i!=futures.end(); i++ ) {
                            shared_ptr<Future::CommandResult> res = *i;
                            if ( ! res->join() ) {
                                error() << "final reduce on sharded output m/r failed on shard: " << res->getServer() << " error: " << res->result() << endl;
                                result.append( "cause" , res->result() );
                                errmsg = "mongod mr failed: ";
                                errmsg += res->result().toString();
                                failed = true;
                                continue;
                            }
                            BSONObj result = res->result();
                        }

                        for ( unsigned i=0; i<shardConns.size(); i++ )
                            shardConns[i]->done();

                        if (failed)
                            return 0;
                    }

                    for ( set<ServerAndQuery>::iterator i=servers.begin(); i!=servers.end(); i++ ) {
                        ScopedDbConnection conn( i->_server );
                        conn->dropCollection( dbName + "." + shardedOutputCollection );
                        conn.done();
                    }

                    result.append("shardCounts", shardCounts);

                    // fix the global counts
                    BSONObjBuilder countsB(32);
                    BSONObjIterator j(aggCounts);
                    while (j.more()) {
                        BSONElement elmt = j.next();
                        if (!strcmp(elmt.fieldName(), "reduce"))
                            countsB.append("reduce", elmt.numberLong() + state.numReduces());
                        else if (!strcmp(elmt.fieldName(), "output"))
                            countsB.append("output", finalCount);
                        else
                            countsB.append(elmt);
                    }
                    result.append( "counts" , countsB.obj() );
                    ok = true;
                }

                if ( ! ok ) {
                    errmsg = "final reduce failed: ";
                    errmsg += finalResult.toString();
                    return 0;
                }
                timingBuilder.append( "final" , t2.millis() );

                result.appendElements( finalResult );
                result.append( "timeMillis" , t.millis() );
                result.append( "timing" , timingBuilder.obj() );

                return 1;
            }
        } mrCmd;

        class ApplyOpsCmd : public PublicGridCommand {
        public:
            ApplyOpsCmd() : PublicGridCommand( "applyOps" ) {}
            virtual bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg = "applyOps not allowed through mongos";
                return false;
            }
        } applyOpsCmd;

        class CompactCmd : public PublicGridCommand {
        public:
            CompactCmd() : PublicGridCommand( "compact" ) {}
            virtual bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg = "compact not allowed through mongos";
                return false;
            }
        } compactCmd;

    }

    bool Command::runAgainstRegistered(const char *ns, BSONObj& jsobj, BSONObjBuilder& anObjBuilder, int queryOptions) {
        const char *p = strchr(ns, '.');
        if ( !p ) return false;
        if ( strcmp(p, ".$cmd") != 0 ) return false;

        bool ok = false;

        BSONElement e = jsobj.firstElement();
        map<string,Command*>::iterator i;

        if ( e.eoo() )
            ;
        // check for properly registered command objects.
        else if ( (i = _commands->find(e.fieldName())) != _commands->end() ) {
            string errmsg;
            Command *c = i->second;
            ClientInfo *client = ClientInfo::get();
            AuthenticationInfo *ai = client->getAuthenticationInfo();

            char cl[256];
            nsToDatabase(ns, cl);
            if( c->requiresAuth() && !ai->isAuthorized(cl)) {
                ok = false;
                errmsg = "unauthorized";
            }
            else if( c->adminOnly() && c->localHostOnlyIfNoAuth( jsobj ) && noauth && !ai->isLocalHost ) {
                ok = false;
                errmsg = "unauthorized: this command must run from localhost when running db without auth";
                log() << "command denied: " << jsobj.toString() << endl;
            }
            else if ( c->adminOnly() && !startsWith(ns, "admin.") ) {
                ok = false;
                errmsg = "access denied - use admin db";
            }
            else if ( jsobj.getBoolField( "help" ) ) {
                stringstream help;
                help << "help for: " << e.fieldName() << " ";
                c->help( help );
                anObjBuilder.append( "help" , help.str() );
            }
            else {
                ok = c->run( nsToDatabase( ns ) , jsobj, queryOptions, errmsg, anObjBuilder, false );
            }

            BSONObj tmp = anObjBuilder.asTempObj();
            bool have_ok = tmp.hasField("ok");
            bool have_errmsg = tmp.hasField("errmsg");

            if (!have_ok)
                anObjBuilder.append( "ok" , ok ? 1.0 : 0.0 );

            if ( !ok && !have_errmsg) {
                anObjBuilder.append("errmsg", errmsg);
                uassert_nothrow(errmsg.c_str());
            }
            return true;
        }

        return false;
    }
}
