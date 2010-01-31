// shardkey.cpp

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
#include "../db/jsobj.h"
#include "../util/unittest.h"

/**
   TODO: this only works with numbers right now
         this is very temporary, need to make work with anything
*/

namespace mongo {
    void minForPat(BSONObjBuilder& out, const BSONObj& pat){
        BSONElement e = pat.firstElement();
        if (e.type() == Object){
            BSONObjBuilder sub;
            minForPat(sub, e.embeddedObject());
            out.append(e.fieldName(), sub.obj());
        } else {
            out.appendMinKey(e.fieldName());
        }
    }

    void maxForPat(BSONObjBuilder& out, const BSONObj& pat){
        BSONElement e = pat.firstElement();
        if (e.type() == Object){
            BSONObjBuilder sub;
            maxForPat(sub, e.embeddedObject());
            out.append(e.fieldName(), sub.obj());
        } else {
            out.appendMaxKey(e.fieldName());
        }
    }

    ShardKeyPattern::ShardKeyPattern( BSONObj p ) : pattern( p.getOwned() ) {
        pattern.getFieldNames(patternfields);

        BSONObjBuilder min;
        minForPat(min, pattern);
        gMin = min.obj();

        BSONObjBuilder max;
        maxForPat(max, pattern);
        gMax = max.obj();
    }

    int ShardKeyPattern::compare( const BSONObj& lObject , const BSONObj& rObject ) {
        BSONObj L = extractKey(lObject);
        uassert( 10198 , "left object doesn't have shard key", !L.isEmpty());
        BSONObj R = extractKey(rObject);
        uassert( 10199 , "right object doesn't have shard key", !R.isEmpty());
        return L.woCompare(R);
    }

    bool ShardKeyPattern::hasShardKey( const BSONObj& obj ) {
        /* this is written s.t. if obj has lots of fields, if the shard key fields are early, 
           it is fast.  so a bit more work to try to be semi-fast.
           */

        for(set<string>::iterator it = patternfields.begin(); it != patternfields.end(); ++it){
            if(obj.getFieldDotted(it->c_str()).eoo())
                return false;
        }
        return true;
    }

    /** @return true if shard s is relevant for query q.

    Example:
     q:     { x : 3 }
     *this: { x : 1 }
     s:     x:2..x:7
       -> true
    */

    bool ShardKeyPattern::relevant(const BSONObj& query, const BSONObj& L, const BSONObj& R) { 
        BSONObj q = extractKey( query );
        if( q.isEmpty() )
            return true;

        BSONElement e = q.firstElement();
        assert( !e.eoo() ) ;

        if( e.type() == RegEx ) {
            /* todo: if starts with ^, we could be smarter here */
            return true;
        }

        if( e.type() == Object ) { 
            BSONObjIterator j(e.embeddedObject());
            BSONElement LE = L.firstElement(); // todo compound keys
            BSONElement RE = R.firstElement(); // todo compound keys
            while( 1 ) { 
                BSONElement f = j.next();
                if( f.eoo() ) 
                    break;
                int op = f.getGtLtOp();
                switch( op ) { 
                    case BSONObj::LT:
                        if( f.woCompare(LE, false) <= 0 )
                            return false;
                        break;
                    case BSONObj::LTE:
                        if( f.woCompare(LE, false) < 0 )
                            return false;
                        break;
                    case BSONObj::GT:
                    case BSONObj::GTE:
                        if( f.woCompare(RE, false) >= 0 )
                            return false;
                        break;
                    case BSONObj::opIN:
                    case BSONObj::NE:
                    case BSONObj::opSIZE:
                        massert( 10423 , "not implemented yet relevant()", false);
                    case BSONObj::Equality:
                        goto normal;
                    default:
                        massert( 10424 , "bad operator in relevant()?", false);
                }
            }
            return true;
        }
normal:
        return L.woCompare(q) <= 0 && R.woCompare(q) > 0;
    }

    bool ShardKeyPattern::relevantForQuery( const BSONObj& query , Chunk * chunk ){
        massert( 10425 , "not done for compound patterns", patternfields.size() == 1);

        bool rel = relevant(query, chunk->getMin(), chunk->getMax());
        if( ! hasShardKey( query ) )
            assert(rel);

        return rel;
    }

    /**
      returns a query that filters results only for the range desired, i.e. returns 
        { $gte : keyval(min), $lt : keyval(max) }
    */
    void ShardKeyPattern::getFilter( BSONObjBuilder& b , const BSONObj& min, const BSONObj& max ){
        massert( 10426 , "not done for compound patterns", patternfields.size() == 1);
        BSONObjBuilder temp;
        temp.appendAs( extractKey(min).firstElement(), "$gte" );
        temp.appendAs( extractKey(max).firstElement(), "$lt" );

        b.append( patternfields.begin()->c_str(), temp.obj() );
    }    

    /**
      Example
      sort:   { ts: -1 }
      *this:  { ts:1 }
      -> -1

      @return
      0 if sort either doesn't have all the fields or has extra fields
      < 0 if sort is descending
      > 1 if sort is ascending
    */
    int ShardKeyPattern::canOrder( const BSONObj& sort ){
        // e.g.:
        //   sort { a : 1 , b : -1 }
        //   pattern { a : -1, b : 1, c : 1 }
        //     -> -1

        int dir = 0;

        BSONObjIterator s(sort);
        BSONObjIterator p(pattern);
        while( 1 ) {
            BSONElement e = s.next();
            if( e.eoo() )
                break;
            if( !p.moreWithEOO() ) 
                return 0;
            BSONElement ep = p.next();
            bool same = e == ep;
            if( !same ) {
                if( strcmp(e.fieldName(), ep.fieldName()) != 0 )
                    return 0;
                // same name, but opposite direction
                if( dir == -1 ) 
                    ;  // ok
                else if( dir == 1 )
                    return 0; // wrong direction for a 2nd field
                else // dir == 0, initial pass
                    dir = -1;
            }
            else { 
                // fields are the same
                if( dir == -1 ) 
                    return 0; // wrong direction
                dir = 1;
            }
        }

        return dir;
    }

    string ShardKeyPattern::toString() const {
        return pattern.toString();
    }

    /* things to test for compound : 
       x hasshardkey 
       _ getFilter (hard?)
       _ relevantForQuery
       x canOrder
       \ middle (deprecating?)
    */
    class ShardKeyUnitTest : public UnitTest {
    public:
        void hasshardkeytest() { 
            BSONObj x = fromjson("{ zid : \"abcdefg\", num: 1.0, name: \"eliot\" }");
            ShardKeyPattern k( BSON( "num" << 1 ) );
            assert( k.hasShardKey(x) );
            assert( !k.hasShardKey( fromjson("{foo:'a'}") ) );

            // try compound key
            {
                ShardKeyPattern k( fromjson("{a:1,b:-1,c:1}") );
                assert( k.hasShardKey( fromjson("{foo:'a',a:'b',c:'z',b:9,k:99}") ) );
                assert( !k.hasShardKey( fromjson("{foo:'a',a:'b',c:'z',bb:9,k:99}") ) );
                assert( !k.hasShardKey( fromjson("{k:99}") ) );
            }

        }
        void rfq() {
            ShardKeyPattern k( BSON( "key" << 1 ) );
            BSONObj q = BSON( "key" << 3 );
            Chunk c(0);
            BSONObj z = fromjson("{ ns : \"alleyinsider.fs.chunks\" , min : {key:2} , max : {key:20} , server : \"localhost:30001\" }");
            c.unserialize(z);
            assert( k.relevantForQuery(q, &c) );
            assert( k.relevantForQuery(fromjson("{foo:9,key:4}"), &c) );
            assert( !k.relevantForQuery(fromjson("{foo:9,key:43}"), &c) );
            assert( k.relevantForQuery(fromjson("{foo:9,key:{$gt:10}}"), &c) );
            assert( !k.relevantForQuery(fromjson("{foo:9,key:{$gt:22}}"), &c) );
            assert( k.relevantForQuery(fromjson("{foo:9}"), &c) );
        }
        void getfilt() { 
            ShardKeyPattern k( BSON( "key" << 1 ) );
            BSONObjBuilder b;
            k.getFilter(b, fromjson("{z:3,key:30}"), fromjson("{key:90}"));
            BSONObj x = fromjson("{ key: { $gte: 30, $lt: 90 } }");
            assert( x.woEqual(b.obj()) );
        }
        void testCanOrder() { 
            ShardKeyPattern k( fromjson("{a:1,b:-1,c:1}") );
            assert( k.canOrder( fromjson("{a:1}") ) == 1 );
            assert( k.canOrder( fromjson("{a:-1}") ) == -1 );
            assert( k.canOrder( fromjson("{a:1,b:-1,c:1}") ) == 1 );
            assert( k.canOrder( fromjson("{a:1,b:1}") ) == 0 );
            assert( k.canOrder( fromjson("{a:-1,b:1}") ) == -1 );
        }
        void extractkeytest() { 
            ShardKeyPattern k( fromjson("{a:1,b:-1,c:1}") );

            BSONObj x = fromjson("{a:1,b:2,c:3}");
            assert( k.extractKey( fromjson("{a:1,b:2,c:3}") ).woEqual(x) );
            assert( k.extractKey( fromjson("{b:2,c:3,a:1}") ).woEqual(x) );
        }
        void run(){
            extractkeytest();

            ShardKeyPattern k( BSON( "key" << 1 ) );
            
            BSONObj min = k.globalMin();

//            cout << min.jsonString(TenGen) << endl;

            BSONObj max = k.globalMax();
            
            BSONObj k1 = BSON( "key" << 5 );

            assert( k.compare( min , max ) < 0 );
            assert( k.compare( min , k1 ) < 0 );
            assert( k.compare( max , min ) > 0 );
            assert( k.compare( min , min ) == 0 );
            
            hasshardkeytest();
            assert( k.hasShardKey( k1 ) );
            assert( ! k.hasShardKey( BSON( "key2" << 1 ) ) );

            BSONObj a = k1;
            BSONObj b = BSON( "key" << 999 );

            assert( k.compare(a,b) < 0 );

            assert( k.canOrder( fromjson("{key:1}") ) == 1 );
            assert( k.canOrder( fromjson("{zz:1}") ) == 0 );
            assert( k.canOrder( fromjson("{key:-1}") ) == -1 );
            
            testCanOrder();
            getfilt();
            rfq();
            // add middle multitype tests
        }
    } shardKeyTest;
    
} // namespace mongo
