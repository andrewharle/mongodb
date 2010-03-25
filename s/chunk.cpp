// shard.cpp

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

#include "stdafx.h"
#include "chunk.h"
#include "config.h"
#include "../util/unittest.h"
#include "../client/connpool.h"
#include "cursors.h"
#include "strategy.h"

namespace mongo {

    // -------  Shard --------

    int Chunk::MaxChunkSize = 1024 * 1204 * 200;
    
    Chunk::Chunk( ChunkManager * manager ) : _manager( manager ){
        _modified = false;
        _lastmod = 0;
        _dataWritten = 0;
    }

    void Chunk::setShard( string s ){
        _shard = s;
        _markModified();
    }
    
    bool Chunk::contains( const BSONObj& obj ) const{
        return
            _manager->getShardKey().compare( getMin() , obj ) <= 0 &&
            _manager->getShardKey().compare( obj , getMax() ) < 0;
    }

    BSONObj Chunk::pickSplitPoint() const{
        int sort = 0;
        
        if ( _manager->getShardKey().globalMin().woCompare( getMin() ) == 0 ){
            sort = 1;
        }
        else if ( _manager->getShardKey().globalMax().woCompare( getMax() ) == 0 ){
            sort = -1;
        }
        
        if ( sort ){
            ScopedDbConnection conn( getShard() );
            Query q;
            if ( sort == 1 )
                q.sort( _manager->getShardKey().key() );
            else {
                BSONObj k = _manager->getShardKey().key();
                BSONObjBuilder r;
                
                BSONObjIterator i(k);
                while( i.more() ) {
                    BSONElement e = i.next();
                    uassert( 10163 ,  "can only handle numbers here - which i think is correct" , e.isNumber() );
                    r.append( e.fieldName() , -1 * e.number() );
                }
                
                q.sort( r.obj() );
            }
            BSONObj end = conn->findOne( _ns , q );
            conn.done();

            if ( ! end.isEmpty() )
                return _manager->getShardKey().extractKey( end );
        }
        
        ScopedDbConnection conn( getShard() );
        BSONObj result;
        if ( ! conn->runCommand( "admin" , BSON( "medianKey" << _ns
                                                 << "keyPattern" << _manager->getShardKey().key()
                                                 << "min" << getMin()
                                                 << "max" << getMax()
                                                 ) , result ) ){
            stringstream ss;
            ss << "medianKey command failed: " << result;
            uassert( 10164 ,  ss.str() , 0 );
        }

        BSONObj median = result.getObjectField( "median" );
        if (median == getMin()){
            //TODO compound support
            BSONElement key = getMin().firstElement();
            BSONObjBuilder b;
            b.appendAs("$gt", key);

            Query q = QUERY(key.fieldName() << b.obj());
            q.sort(_manager->getShardKey().key());

            median = conn->findOne(_ns, q);
            median = _manager->getShardKey().extractKey( median );
            PRINT(median);
        }

        conn.done();
        
        return median.getOwned();
    }

    Chunk * Chunk::split(){
        return split( pickSplitPoint() );
    }
    
    Chunk * Chunk::split( const BSONObj& m ){
        uassert( 10165 ,  "can't split as shard that doesn't have a manager" , _manager );
        
        log(1) << " before split on: "  << m << "\n"
               << "\t self  : " << toString() << endl;

        uassert( 10166 ,  "locking namespace on server failed" , lockNamespaceOnServer( getShard() , _ns ) );
        uassert( 13003 ,  "can't split chunk. does it have only one distinct value?" ,
                          !m.isEmpty() && _min.woCompare(m) && _max.woCompare(m)); 

        Chunk * s = new Chunk( _manager );
        s->_ns = _ns;
        s->_shard = _shard;
        s->setMin(m.getOwned());
        s->setMax(_max);
        
        s->_markModified();
        _markModified();
        
        _manager->_chunks.push_back( s );
        
        setMax(m.getOwned());
        
        log(1) << " after split:\n" 
               << "\t left : " << toString() << "\n" 
               << "\t right: "<< s->toString() << endl;
        
        
        _manager->save();
        
        return s;
    }

    bool Chunk::moveAndCommit( const string& to , string& errmsg ){
        uassert( 10167 ,  "can't move shard to its current location!" , to != getShard() );

        log() << "moving chunk ns: " << _ns << " moving chunk: " << toString() << " " << _shard << " -> " << to << endl;
        
        string from = _shard;
        ShardChunkVersion oldVersion = _manager->getVersion( from );
        
        BSONObj filter;
        {
            BSONObjBuilder b;
            getFilter( b );
            filter = b.obj();
        }
        
        ScopedDbConnection fromconn( from );

        BSONObj startRes;
        bool worked = fromconn->runCommand( "admin" ,
                                            BSON( "movechunk.start" << _ns << 
                                                  "from" << from <<
                                                  "to" << to <<
                                                  "filter" << filter
                                                  ) ,
                                            startRes
                                            );
        
        if ( ! worked ){
            errmsg = (string)"movechunk.start failed: " + startRes.toString();
            fromconn.done();
            return false;
        }
        
        // update config db
        setShard( to );
        
        // need to increment version # for old server
        Chunk * randomChunkOnOldServer = _manager->findChunkOnServer( from );
        if ( randomChunkOnOldServer )
            randomChunkOnOldServer->_markModified();
        
        _manager->save();
        
        BSONObj finishRes;
        {

            ShardChunkVersion newVersion = _manager->getVersion( from );
            if ( newVersion == 0 && oldVersion > 0 ){
                newVersion = oldVersion;
                newVersion++;
                _manager->save();
            }
            else if ( newVersion <= oldVersion ){
                log() << "newVersion: " << newVersion << " oldVersion: " << oldVersion << endl;
                uassert( 10168 ,  "version has to be higher" , newVersion > oldVersion );
            }
            
            BSONObjBuilder b;
            b << "movechunk.finish" << _ns;
            b << "to" << to;
            b.appendTimestamp( "newVersion" , newVersion );
            b.append( startRes["finishToken"] );
        
            worked = fromconn->runCommand( "admin" ,
                                           b.done() , 
                                           finishRes );
        }
        
        if ( ! worked ){
            errmsg = (string)"movechunk.finish failed: " + finishRes.toString();
            fromconn.done();
            return false;
        }
        
        fromconn.done();
        return true;
    }
    
    bool Chunk::splitIfShould( long dataWritten ){
        _dataWritten += dataWritten;
        
        if ( _dataWritten < MaxChunkSize / 5 )
            return false;
        
        log(1) << "\t want to split chunk : " << this << endl;

        _dataWritten = 0;
        
        BSONObj split_point = pickSplitPoint();
        if ( split_point.isEmpty() || _min == split_point || _max == split_point) {
            log() << "SHARD PROBLEM** shard is too big, but can't split: " << toString() << endl;
            return false;
        }

        long size = getPhysicalSize();
        if ( size < MaxChunkSize )
            return false;
        
        log() << "autosplitting " << _ns << " size: " << size << " shard: " << toString() << endl;
        Chunk * newShard = split(split_point);

        moveIfShould( newShard );
        
        return true;
    }

    bool Chunk::moveIfShould( Chunk * newChunk ){
        Chunk * toMove = 0;
       
        if ( newChunk->countObjects() <= 1 ){
            toMove = newChunk;
        }
        else if ( this->countObjects() <= 1 ){
            toMove = this;
        }
        else {
            log(1) << "don't know how to decide if i should move inner shard" << endl;
        }

        if ( ! toMove )
            return false;
        
        string newLocation = grid.pickShardForNewDB();
        if ( newLocation == getShard() ){
            // if this is the best server, then we shouldn't do anything!
            log(1) << "not moving chunk: " << toString() << " b/c would move to same place  " << newLocation << " -> " << getShard() << endl;
            return 0;
        }

        log() << "moving chunk (auto): " << toMove->toString() << " to: " << newLocation << " #objcets: " << toMove->countObjects() << endl;

        string errmsg;
        massert( 10412 ,  (string)"moveAndCommit failed: " + errmsg , 
                 toMove->moveAndCommit( newLocation , errmsg ) );
        
        return true;
    }

    long Chunk::getPhysicalSize() const{
        ScopedDbConnection conn( getShard() );
        
        BSONObj result;
        uassert( 10169 ,  "datasize failed!" , conn->runCommand( "admin" , BSON( "datasize" << _ns
                                                                        << "keyPattern" << _manager->getShardKey().key() 
                                                                        << "min" << getMin() 
                                                                        << "max" << getMax() 
                                                                        ) , result ) );
        
        conn.done();
        return (long)result["size"].number();
    }

    
    long Chunk::countObjects( const BSONObj& filter ) const{
        ScopedDbConnection conn( getShard() );
        
        BSONObj f = getFilter();
        if ( ! filter.isEmpty() )
            f = ClusteredCursor::concatQuery( f , filter );

        BSONObj result;
        unsigned long long n = conn->count( _ns , f );
        
        conn.done();
        return (long)n;
    }
    
    bool Chunk::operator==( const Chunk& s ) const{
        return 
            _manager->getShardKey().compare( _min , s._min ) == 0 &&
            _manager->getShardKey().compare( _max , s._max ) == 0
            ;
    }

    void Chunk::getFilter( BSONObjBuilder& b ) const{
        _manager->_key.getFilter( b , _min , _max );
    }
    
    void Chunk::serialize(BSONObjBuilder& to){
        if ( _lastmod )
            to.appendTimestamp( "lastmod" , _lastmod );
        else 
            to.appendTimestamp( "lastmod" );

        to << "ns" << _ns;
        to << "min" << _min;
        to << "max" << _max;
        to << "shard" << _shard;
    }
    
    void Chunk::unserialize(const BSONObj& from){
        _ns = from.getStringField( "ns" );
        _shard = from.getStringField( "shard" );
        _lastmod = from.hasField( "lastmod" ) ? from["lastmod"]._numberLong() : 0;

        BSONElement e = from["minDotted"];
        cout << from << endl;
        if (e.eoo()){
            _min = from.getObjectField( "min" ).getOwned();
            _max = from.getObjectField( "max" ).getOwned();
        } else { // TODO delete this case after giving people a chance to migrate
            _min = e.embeddedObject().getOwned();
            _max = from.getObjectField( "maxDotted" ).getOwned();
        }
        
        uassert( 10170 ,  "Chunk needs a ns" , ! _ns.empty() );
        uassert( 10171 ,  "Chunk needs a server" , ! _ns.empty() );

        uassert( 10172 ,  "Chunk needs a min" , ! _min.isEmpty() );
        uassert( 10173 ,  "Chunk needs a max" , ! _max.isEmpty() );
    }

    string Chunk::modelServer() {
        // TODO: this could move around?
        return configServer.modelServer();
    }
    
    void Chunk::_markModified(){
        _modified = true;
        // set to 0 so that the config server sets it
        _lastmod = 0;
    }

    void Chunk::save( bool check ){
        bool reload = ! _lastmod;
        Model::save( check );
        if ( reload ){
            // need to do this so that we get the new _lastMod and therefore version number
            massert( 10413 ,  "_id has to be filled in already" , ! _id.isEmpty() );
            
            string b = toString();
            BSONObj q = _id.copy();
            massert( 10414 ,  "how could load fail?" , load( q ) );
            log(2) << "before: " << q << "\t" << b << endl;
            log(2) << "after : " << _id << "\t" << toString() << endl;
            massert( 10415 ,  "chunk reload changed content!" , b == toString() );
            massert( 10416 ,  "id changed!" , q["_id"] == _id["_id"] );
        }
    }
    
    void Chunk::ensureIndex(){
        ScopedDbConnection conn( getShard() );
        conn->ensureIndex( _ns , _manager->getShardKey().key() , _manager->_unique );
        conn.done();
    }

    string Chunk::toString() const {
        stringstream ss;
        ss << "shard  ns:" << _ns << " shard: " << _shard << " min: " << _min << " max: " << _max;
        return ss.str();
    }
    
    
    ShardKeyPattern Chunk::skey() const{
        return _manager->getShardKey();
    }

    // -------  ChunkManager --------

    unsigned long long ChunkManager::NextSequenceNumber = 1;

    ChunkManager::ChunkManager( DBConfig * config , string ns , ShardKeyPattern pattern , bool unique ) : 
        _config( config ) , _ns( ns ) , _key( pattern ) , _unique( unique ){
        Chunk temp(0);
        
        ScopedDbConnection conn( temp.modelServer() );
        auto_ptr<DBClientCursor> cursor = conn->query( temp.getNS() , BSON( "ns" <<  ns ) );
        while ( cursor->more() ){
            BSONObj d = cursor->next();
            if ( d["isMaxMarker"].trueValue() ){
                continue;
            }

            Chunk * c = new Chunk( this );
            c->unserialize( d );
            _chunks.push_back( c );
            c->_id = d["_id"].wrap().getOwned();
        }
        conn.done();
        
        if ( _chunks.size() == 0 ){
            Chunk * c = new Chunk( this );
            c->_ns = ns;
            c->setMin(_key.globalMin());
            c->setMax(_key.globalMax());
            c->_shard = config->getPrimary();
            c->_markModified();
            
            _chunks.push_back( c );
            
            log() << "no chunks for:" << ns << " so creating first: " << c->toString() << endl;
        }

        _sequenceNumber = ++NextSequenceNumber;
    }
    
    ChunkManager::~ChunkManager(){
        for ( vector<Chunk*>::iterator i=_chunks.begin(); i != _chunks.end(); i++ ){
            delete( *i );
        }
        _chunks.clear();
    }

    bool ChunkManager::hasShardKey( const BSONObj& obj ){
        return _key.hasShardKey( obj );
    }

    Chunk& ChunkManager::findChunk( const BSONObj & obj ){
        
        for ( vector<Chunk*>::iterator i=_chunks.begin(); i != _chunks.end(); i++ ){
            Chunk * c = *i;
            if ( c->contains( obj ) )
                return *c;
        }
        stringstream ss;
        ss << "couldn't find a chunk which should be impossible  extracted: " << _key.extractKey( obj );
        throw UserException( 8070 , ss.str() );
    }

    Chunk* ChunkManager::findChunkOnServer( const string& server ) const {

        for ( vector<Chunk*>::const_iterator i=_chunks.begin(); i!=_chunks.end(); i++ ){
            Chunk * c = *i;
            if ( c->getShard() == server )
                return c;
        }

        return 0;
    }

    int ChunkManager::getChunksForQuery( vector<Chunk*>& chunks , const BSONObj& query ){
        int added = 0;
        
        for ( vector<Chunk*>::iterator i=_chunks.begin(); i != _chunks.end(); i++  ){
            Chunk * c = *i;
            if ( _key.relevantForQuery( query , c ) ){
                chunks.push_back( c );
                added++;
            }
        }
        return added;
    }

    void ChunkManager::getAllServers( set<string>& allServers ){
        for ( vector<Chunk*>::iterator i=_chunks.begin(); i != _chunks.end(); i++  ){
            allServers.insert( (*i)->getShard() );
        }        
    }
    
    void ChunkManager::ensureIndex(){
        set<string> seen;
        
        for ( vector<Chunk*>::const_iterator i=_chunks.begin(); i!=_chunks.end(); i++ ){
            Chunk * c = *i;
            if ( seen.count( c->getShard() ) )
                continue;
            seen.insert( c->getShard() );
            c->ensureIndex();
        }
    }
    
    void ChunkManager::drop(){
        uassert( 10174 ,  "config servers not all up" , configServer.allUp() );
        
        map<string,ShardChunkVersion> seen;
        
        log(1) << "ChunkManager::drop : " << _ns << endl;

        // lock all shards so no one can do a split/migrate
        for ( vector<Chunk*>::const_iterator i=_chunks.begin(); i!=_chunks.end(); i++ ){
            Chunk * c = *i;
            ShardChunkVersion& version = seen[ c->getShard() ];
            if ( version ) 
                continue;
            version = lockNamespaceOnServer( c->getShard() , _ns );
            if ( version )
                continue;

            // rollback
            uassert( 10175 ,  "don't know how to rollback locks b/c drop can't lock all shards" , 0 );
        }
        
        log(1) << "ChunkManager::drop : " << _ns << "\t all locked" << endl;        

        // wipe my meta-data
        _chunks.clear();

        
        // delete data from mongod
        for ( map<string,ShardChunkVersion>::iterator i=seen.begin(); i!=seen.end(); i++ ){
            string shard = i->first;
            ScopedDbConnection conn( shard );
            conn->dropCollection( _ns );
            conn.done();
        }
        
        log(1) << "ChunkManager::drop : " << _ns << "\t removed shard data" << endl;        

        // clean up database meta-data
        uassert( 10176 ,  "no sharding data?" , _config->removeSharding( _ns ) );
        _config->save();
        
        
        // remove chunk data
        Chunk temp(0);
        ScopedDbConnection conn( temp.modelServer() );
        conn->remove( temp.getNS() , BSON( "ns" << _ns ) );
        conn.done();
        log(1) << "ChunkManager::drop : " << _ns << "\t removed chunk data" << endl;                
        
        for ( map<string,ShardChunkVersion>::iterator i=seen.begin(); i!=seen.end(); i++ ){
            ScopedDbConnection conn( i->first );
            BSONObj res;
            if ( ! setShardVersion( conn.conn() , _ns , 0 , true , res ) )
                throw UserException( 8071 , (string)"OH KNOW, cleaning up after drop failed: " + res.toString() );
            conn.done();
        }


        log(1) << "ChunkManager::drop : " << _ns << "\t DONE" << endl;        
    }
    
    void ChunkManager::save(){
        ShardChunkVersion a = getVersion();
        
        set<string> withRealChunks;
        
        for ( vector<Chunk*>::const_iterator i=_chunks.begin(); i!=_chunks.end(); i++ ){
            Chunk* c = *i;
            if ( ! c->_modified )
                continue;
            c->save( true );
            _sequenceNumber = ++NextSequenceNumber;

            withRealChunks.insert( c->getShard() );
        }
        
        massert( 10417 ,  "how did version get smalled" , getVersion() >= a );

        ensureIndex(); // TODO: this is too aggressive - but not really sooo bad
    }
    
    ShardChunkVersion ChunkManager::getVersion( const string& server ) const{
        // TODO: cache or something?
        
        ShardChunkVersion max = 0;

        for ( vector<Chunk*>::const_iterator i=_chunks.begin(); i!=_chunks.end(); i++ ){
            Chunk* c = *i;
            if ( c->getShard() != server )
                continue;
            
            if ( c->_lastmod > max )
                max = c->_lastmod;
        }        
        
        return max;
    }

    ShardChunkVersion ChunkManager::getVersion() const{
        ShardChunkVersion max = 0;

        for ( vector<Chunk*>::const_iterator i=_chunks.begin(); i!=_chunks.end(); i++ ){
            Chunk* c = *i;
            if ( c->_lastmod > max )
                max = c->_lastmod;
        }        

        return max;
    }

    string ChunkManager::toString() const {
        stringstream ss;
        ss << "ChunkManager: " << _ns << " key:" << _key.toString() << "\n";
        for ( vector<Chunk*>::const_iterator i=_chunks.begin(); i!=_chunks.end(); i++ ){
            const Chunk* c = *i;
            ss << "\t" << c->toString() << "\n";
        }
        return ss.str();
    }
    
    
    class ChunkObjUnitTest : public UnitTest {
    public:
        void runShard(){

        }
        
        void run(){
            runShard();
            log(1) << "shardObjTest passed" << endl;
        }
    } shardObjTest;


} // namespace mongo
