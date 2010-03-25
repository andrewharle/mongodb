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

// strategy_sharded.cpp

#include "stdafx.h"
#include "request.h"
#include "chunk.h"
#include "cursors.h"
#include "../client/connpool.h"
#include "../db/commands.h"

// error codes 8010-8040

namespace mongo {
    
    class ShardStrategy : public Strategy {

        virtual void queryOp( Request& r ){
            QueryMessage q( r.d() );

            log(3) << "shard query: " << q.ns << "  " << q.query << endl;
            
            if ( q.ntoreturn == 1 && strstr(q.ns, ".$cmd") )
                throw UserException( 8010 , "something is wrong, shouldn't see a command here" );

            ChunkManager * info = r.getChunkManager();
            assert( info );
            
            Query query( q.query );

            vector<Chunk*> shards;
            info->getChunksForQuery( shards , query.getFilter()  );
            
            set<ServerAndQuery> servers;
            map<string,int> serverCounts;
            for ( vector<Chunk*>::iterator i = shards.begin(); i != shards.end(); i++ ){
                servers.insert( ServerAndQuery( (*i)->getShard() , (*i)->getFilter() ) );
                int& num = serverCounts[(*i)->getShard()];
                num++;
            }
            
            if ( logLevel > 4 ){
                StringBuilder ss;
                ss << " shard query servers: " << servers.size() << "\n";
                for ( set<ServerAndQuery>::iterator i = servers.begin(); i!=servers.end(); i++ ){
                    const ServerAndQuery& s = *i;
                    ss << "       " << s.toString() << "\n";
                }
                log() << ss.str();
            }

            ClusteredCursor * cursor = 0;
            
            BSONObj sort = query.getSort();
            
            if ( sort.isEmpty() ){
                // 1. no sort, can just hit them in serial
                cursor = new SerialServerClusteredCursor( servers , q );
            }
            else {
                int shardKeyOrder = info->getShardKey().canOrder( sort );
                if ( shardKeyOrder ){
                    // 2. sort on shard key, can do in serial intelligently
                    set<ServerAndQuery> buckets;
                    for ( vector<Chunk*>::iterator i = shards.begin(); i != shards.end(); i++ ){
                        Chunk * s = *i;
                        buckets.insert( ServerAndQuery( s->getShard() , s->getFilter() , s->getMin() ) );
                    }
                    cursor = new SerialServerClusteredCursor( buckets , q , shardKeyOrder );
                }
                else {
                    // 3. sort on non-sharded key, pull back a portion from each server and iterate slowly
                    cursor = new ParallelSortClusteredCursor( servers , q , sort );
                }
            }

            assert( cursor );
            
            log(5) << "   cursor type: " << cursor->type() << endl;

            ShardedClientCursor * cc = new ShardedClientCursor( q , cursor );
            if ( ! cc->sendNextBatch( r ) ){
                delete( cursor );
                return;
            }
            log(6) << "storing cursor : " << cc->getId() << endl;
            cursorCache.store( cc );
        }
        
        virtual void getMore( Request& r ){
            int ntoreturn = r.d().pullInt();
            long long id = r.d().pullInt64();

            log(6) << "want cursor : " << id << endl;

            ShardedClientCursor * cursor = cursorCache.get( id );
            if ( ! cursor ){
                log(6) << "\t invalid cursor :(" << endl;
                replyToQuery( QueryResult::ResultFlag_CursorNotFound , r.p() , r.m() , 0 , 0 , 0 );
                return;
            }
            
            if ( cursor->sendNextBatch( r , ntoreturn ) ){
                log(6) << "\t cursor finished: " << id << endl;
                return;
            }
            
            delete( cursor );
            cursorCache.remove( id );
        }
        
        void _insert( Request& r , DbMessage& d, ChunkManager* manager ){
            
            while ( d.moreJSObjs() ){
                BSONObj o = d.nextJsObj();
                if ( ! manager->hasShardKey( o ) ){

                    bool bad = true;

                    if ( manager->getShardKey().partOfShardKey( "_id" ) ){
                        BSONObjBuilder b;
                        b.appendOID( "_id" , 0 , true );
                        b.appendElements( o );
                        o = b.obj();
                        bad = ! manager->hasShardKey( o );
                    }
                    
                    if ( bad ){
                        log() << "tried to insert object without shard key: " << r.getns() << "  " << o << endl;
                        throw UserException( 8011 , "tried to insert object without shard key" );
                    }
                    
                }
                
                Chunk& c = manager->findChunk( o );
                log(4) << "  server:" << c.getShard() << " " << o << endl;
                insert( c.getShard() , r.getns() , o );
                
                c.splitIfShould( o.objsize() );
            }            
        }

        void _update( Request& r , DbMessage& d, ChunkManager* manager ){
            int flags = d.pullInt();
            
            BSONObj query = d.nextJsObj();
            uassert( 10201 ,  "invalid update" , d.moreJSObjs() );
            BSONObj toupdate = d.nextJsObj();

            BSONObj chunkFinder = query;
            
            bool upsert = flags & UpdateOption_Upsert;
            bool multi = flags & UpdateOption_Multi;

            if ( multi )
                uassert( 10202 ,  "can't mix multi and upsert and sharding" , ! upsert );

            if ( upsert && !(manager->hasShardKey(toupdate) ||
                             (toupdate.firstElement().fieldName()[0] == '$' && manager->hasShardKey(query))))
            {
                throw UserException( 8012 , "can't upsert something without shard key" );
            }

            bool save = false;
            if ( ! manager->hasShardKey( query ) ){
                if ( multi ){
                }
                else if ( query.nFields() != 1 || strcmp( query.firstElement().fieldName() , "_id" ) ){
                    throw UserException( 8013 , "can't do update with query that doesn't have the shard key" );
                }
                else {
                    save = true;
                    chunkFinder = toupdate;
                }
            }

            
            if ( ! save ){
                if ( toupdate.firstElement().fieldName()[0] == '$' ){
                    // TODO: check for $set, etc.. on shard key
                }
                else if ( manager->hasShardKey( toupdate ) && manager->getShardKey().compare( query , toupdate ) ){
                    throw UserException( 8014 , "change would move shards!" );
                }
            }
            
            if ( multi ){
                vector<Chunk*> chunks;
                manager->getChunksForQuery( chunks , chunkFinder );
                set<string> seen;
                for ( vector<Chunk*>::iterator i=chunks.begin(); i!=chunks.end(); i++){
                    Chunk * c = *i;
                    if ( seen.count( c->getShard() ) )
                        continue;
                    doWrite( dbUpdate , r , c->getShard() );
                    seen.insert( c->getShard() );
                }
            }
            else {
                Chunk& c = manager->findChunk( chunkFinder );
                doWrite( dbUpdate , r , c.getShard() );
                c.splitIfShould( d.msg().data->dataLen() );
            }

        }
        
        void _delete( Request& r , DbMessage& d, ChunkManager* manager ){

            int flags = d.pullInt();
            bool justOne = flags & 1;
            
            uassert( 10203 ,  "bad delete message" , d.moreJSObjs() );
            BSONObj pattern = d.nextJsObj();

            vector<Chunk*> chunks;
            manager->getChunksForQuery( chunks , pattern );
            cout << "delete : " << pattern << " \t " << chunks.size() << " justOne: " << justOne << endl;
            if ( chunks.size() == 1 ){
                doWrite( dbDelete , r , chunks[0]->getShard() );
                return;
            }
            
            if ( justOne && ! pattern.hasField( "_id" ) )
                throw UserException( 8015 , "can only delete with a non-shard key pattern if can delete as many as we find" );
            
            set<string> seen;
            for ( vector<Chunk*>::iterator i=chunks.begin(); i!=chunks.end(); i++){
                Chunk * c = *i;
                if ( seen.count( c->getShard() ) )
                    continue;
                seen.insert( c->getShard() );
                doWrite( dbDelete , r , c->getShard() );
            }
        }
        
        virtual void writeOp( int op , Request& r ){
            const char *ns = r.getns();
            log(3) << "write: " << ns << endl;
            
            DbMessage& d = r.d();
            ChunkManager * info = r.getChunkManager();
            assert( info );
            
            if ( op == dbInsert ){
                _insert( r , d , info );
            }
            else if ( op == dbUpdate ){
                _update( r , d , info );    
            }
            else if ( op == dbDelete ){
                _delete( r , d , info );
            }
            else {
                log() << "sharding can't do write op: " << op << endl;
                throw UserException( 8016 , "can't do this write op on sharded collection" );
            }
            
        }
    };
    
    Strategy * SHARDED = new ShardStrategy();
}
