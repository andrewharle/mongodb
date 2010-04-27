// geo2d.cpp

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
#include "namespace.h"
#include "jsobj.h"
#include "index.h"
#include "../util/unittest.h"
#include "commands.h"
#include "pdfile.h"
#include "btree.h"
#include "curop.h"
#include "matcher.h"

//#define GEODEBUG(x) cout << x << endl;
#define GEODEBUG(x) 

namespace mongo {

    const string GEO2DNAME = "2d";

    class GeoBitSets {
    public:
        GeoBitSets(){
            for ( int i=0; i<32; i++ ){
                masks32[i] = ( 1 << ( 31 - i ) );
            }
            for ( int i=0; i<64; i++ ){
                masks64[i] = ( 1LL << ( 63 - i ) );
            }
        }
        int masks32[32];
        long long masks64[64];
    } geoBitSets;

    
    class GeoHash {
    public:
        GeoHash()
            : _hash(0),_bits(0){
        }

        GeoHash( const char * hash ){
            init( hash );
        }

        GeoHash( const string& hash ){
            init( hash );
        }

        GeoHash( const BSONElement& e , unsigned bits=32 ){
            _bits = bits;
            if ( e.type() == BinData ){
                int len = 0;
                _copy( (char*)&_hash , e.binData( len ) );
                assert( len == 8 );
                _bits = bits;
            }
            else {
                cout << "GeoHash cons e : " << e << endl;
                uassert(13047,"wrong type for geo index. if you're using a pre-release version, need to rebuild index",0);
            }
            _fix();
        }
        
        GeoHash( unsigned x , unsigned y , unsigned bits=32){
            init( x , y , bits );
        }

        GeoHash( const GeoHash& old ){
            _hash = old._hash;
            _bits = old._bits;
        }

        GeoHash( long long hash , unsigned bits )
            : _hash( hash ) , _bits( bits ){
            _fix();
        }

        void init( unsigned x , unsigned y , unsigned bits ){
            assert( bits <= 32 );
            _hash = 0;
            _bits = bits;
            for ( unsigned i=0; i<bits; i++ ){
                if ( isBitSet( x , i ) ) _hash |= geoBitSets.masks64[i*2];
                if ( isBitSet( y , i ) ) _hash |= geoBitSets.masks64[(i*2)+1];
            }
        }

        void unhash( unsigned& x , unsigned& y ) const {
            x = 0;
            y = 0;
            for ( unsigned i=0; i<_bits; i++ ){
                if ( getBitX(i) )
                    x |= geoBitSets.masks32[i];
                if ( getBitY(i) )
                    y |= geoBitSets.masks32[i];
            }
        }

        /**
         * @param 0 = high
         */
        static bool isBitSet( unsigned val , unsigned  bit ){
            return geoBitSets.masks32[bit] & val;
        }
        
        GeoHash up() const {
            return GeoHash( _hash , _bits - 1 );
        }
        
        bool hasPrefix( const GeoHash& other ) const {
            assert( other._bits <= _bits );
            if ( other._bits == 0 )
                return true;
            long long x = other._hash ^ _hash;
            x = x >> (64-(other._bits*2));
            return x == 0;
        }
        

        string toString() const { 
            StringBuilder buf( _bits * 2 );
            for ( unsigned x=0; x<_bits*2; x++ )
                buf.append( _hash & geoBitSets.masks64[x] ? "1" : "0" );
            return buf.str();
        }

        string toStringHex1() const {
            stringstream ss;
            ss << hex << _hash;
            return ss.str();
        }

        void init( const string& s ){
            _hash = 0;
            _bits = s.size() / 2;
            for ( unsigned pos=0; pos<s.size(); pos++ )
                if ( s[pos] == '1' )
                    setBit( pos , 1 );
        }

        void setBit( unsigned pos , bool one ){
            assert( pos < _bits * 2 );
            if ( one )
                _hash |= geoBitSets.masks64[pos];
            else if ( _hash & geoBitSets.masks64[pos] )
                _hash &= ~geoBitSets.masks64[pos];
        }
        
        bool getBit( unsigned pos ) const {
            return _hash & geoBitSets.masks64[pos];
        }

        bool getBitX( unsigned pos ) const {
            assert( pos < 32 );
            return getBit( pos * 2 );
        }

        bool getBitY( unsigned pos ) const {
            assert( pos < 32 );
            return getBit( ( pos * 2 ) + 1 );
        }
        
        BSONObj wrap() const {
            BSONObjBuilder b(20);
            append( b , "" );
            BSONObj o = b.obj();
            assert( o.objsize() == 20 );
            return o;
        }

        bool constrains() const {
            return _bits > 0;
        }
        
        void move( int x , int y ){
            assert( _bits );
            _move( 0 , x );
            _move( 1 , y );
        }

        void _move( unsigned offset , int d ){
            if ( d == 0 )
                return;
            assert( d <= 1 && d>= -1 ); // TEMP
            
            bool from, to;
            if ( d > 0 ){
                from = 0;
                to = 1;
            }
            else {
                from = 1;
                to = 0;
            }

            unsigned pos = ( _bits * 2 ) - 1;
            if ( offset == 0 )
                pos--;
            while ( true ){
                if ( getBit(pos) == from ){
                    setBit( pos , to );
                    return;
                }

                if ( pos < 2 ){
                    // overflow
                    for ( ; pos < ( _bits * 2 ) ; pos += 2 ){
                        setBit( pos , from );
                    }
                    return;
                }
                
                setBit( pos , from );
                pos -= 2;
            }
            
            assert(0);
        }

        GeoHash& operator=(const GeoHash& h) { 
            _hash = h._hash;
            _bits = h._bits;
            return *this;
        }
        
        bool operator==(const GeoHash& h ){
            return _hash == h._hash && _bits == h._bits;
        }

        GeoHash& operator+=( const char * s ) {
            unsigned pos = _bits * 2;
            _bits += strlen(s) / 2;
            assert( _bits <= 32 );
            while ( s[0] ){
                if ( s[0] == '1' )
                    setBit( pos , 1 );
                pos++;
                s++;
            }

            return *this;
        }

        GeoHash operator+( const char * s ) const {
            GeoHash n = *this;
            n+=s;
            return n;
        }
      
        void _fix(){
            if ( ( _hash << ( _bits * 2 ) ) == 0 )
                return;
            long long mask = 0;
            for ( unsigned i=0; i<_bits*2; i++ )
                mask |= geoBitSets.masks64[i];
            _hash &= mask;
        }
        
        void append( BSONObjBuilder& b , const char * name ) const {
            char buf[8];
            _copy( buf , (char*)&_hash );
            b.appendBinData( name , 8 , bdtCustom , buf );
        }
        
        long long getHash() const {
            return _hash;
        }

        GeoHash commonPrefix( const GeoHash& other ) const {
            unsigned i=0;
            for ( ; i<_bits && i<other._bits; i++ ){
                if ( getBitX( i ) == other.getBitX( i ) &&
                     getBitY( i ) == other.getBitY( i ) )
                    continue;
                break;
            }
            return GeoHash(_hash,i);
        }
    private:

        void _copy( char * dst , const char * src ) const {
            for ( unsigned a=0; a<8; a++ ){
                dst[a] = src[7-a];
            }
        }

        long long _hash;
        unsigned _bits; // bits per field, so 1 to 32
    };

    ostream& operator<<( ostream &s, const GeoHash &h ){
        s << h.toString();
        return s;
    } // end GeoHash

    class Geo2dType : public IndexType {
    public:
        Geo2dType( const IndexPlugin * plugin , const IndexSpec* spec )
            : IndexType( plugin , spec ){
            
            BSONObjBuilder orderBuilder;

            BSONObjIterator i( spec->keyPattern );
            while ( i.more() ){
                BSONElement e = i.next();
                if ( e.type() == String && GEO2DNAME == e.valuestr() ){
                    uassert( 13022 , "can't have 2 geo field" , _geo.size() == 0 );
                    uassert( 13023 , "2d has to be first in index" , _other.size() == 0 );
                    _geo = e.fieldName();
                }
                else {
                    _other.push_back( e.fieldName() );
                }
                orderBuilder.append( "" , 1 );
            }
            
            uassert( 13024 , "no geo field specified" , _geo.size() );
            
            _bits = _configval( spec , "bits" , 26 ); // for lat/long, ~ 1ft

            uassert( 13028 , "can't have more than 32 bits in geo index" , _bits <= 32 );

            _max = _configval( spec , "max" , 180 );
            _min = _configval( spec , "min" , -180 );
            
            _scaling = (1024*1024*1024*4.0)/(_max-_min);

            _order = orderBuilder.obj();
        }

        int _configval( const IndexSpec* spec , const string& name , int def ){
            BSONElement e = spec->info[name];
            if ( e.isNumber() )
                return e.numberInt();
            return def;
        }

        ~Geo2dType(){
            
        }

        virtual BSONObj fixKey( const BSONObj& in ) { 
            if ( in.firstElement().type() == BinData )
                return in;

            BSONObjBuilder b(in.objsize()+16);
            
            if ( in.firstElement().isABSONObj() )
                _hash( in.firstElement().embeddedObject() ).append( b , "" );
            else if ( in.firstElement().type() == String )
                GeoHash( in.firstElement().valuestr() ).append( b , "" );
            else if ( in.firstElement().type() == RegEx )
                GeoHash( in.firstElement().regex() ).append( b , "" );
            else 
                return in;

            BSONObjIterator i(in);
            i.next();
            while ( i.more() )
                b.append( i.next() );
            return b.obj();
        }

        virtual void getKeys( const BSONObj &obj, BSONObjSetDefaultOrder &keys ) const {
            BSONElement geo = obj.getFieldDotted(_geo.c_str());
            if ( geo.eoo() )
                return;

            BSONObjBuilder b(64);

            if ( ! geo.isABSONObj() )
                return;

            BSONObj embed = geo.embeddedObject();
            if ( embed.isEmpty() )
                return;

            _hash( embed ).append( b , "" );

            for ( size_t i=0; i<_other.size(); i++ ){
                BSONElement e = obj[_other[i]];
                if ( e.eoo() )
                    e = _spec->missingField();
                b.appendAs( e , "" );
            }
            keys.insert( b.obj() );
        }
        
        GeoHash _tohash( const BSONElement& e ) const {
            if ( e.isABSONObj() )
                return _hash( e.embeddedObject() );
            
            return GeoHash( e , _bits );
        }

        GeoHash _hash( const BSONObj& o ) const {
            BSONObjIterator i(o);
            uassert( 13067 , "geo field is empty" , i.more() );
            BSONElement x = i.next();
            uassert( 13068 , "geo field only has 1 element" , i.more() );
            BSONElement y = i.next();
            
            uassert( 13026 , "geo values have to be numbers" , x.isNumber() && y.isNumber() );

            return _hash( x.number() , y.number() );
        }

        GeoHash _hash( double x , double y ) const {
            return GeoHash( _convert(x), _convert(y) , _bits );
        }

        BSONObj _unhash( const GeoHash& h ) const {
            unsigned x , y;
            h.unhash( x , y );
            BSONObjBuilder b;
            b.append( "x" , _unconvert( x ) );
            b.append( "y" , _unconvert( y ) );
            return b.obj();
        }
        
        unsigned _convert( double in ) const {
            uassert( 13027 , "point not in range" , in <= _max && in >= _min );
            in -= _min;
            assert( in > 0 );
            return (unsigned)(in * _scaling);
        }
        
        double _unconvert( unsigned in ) const {
            double x = in;
            x /= _scaling;
            x += _min;
            return x;
        }

        void _unconvert( const GeoHash& h , double& x , double& y ) const {
            unsigned a,b;
            h.unhash(a,b);
            x = _unconvert( a );
            y = _unconvert( b );
        }
        
        double distance( const GeoHash& a , const GeoHash& b ) const {
            double ax,ay,bx,by;
            _unconvert( a , ax , ay );
            _unconvert( b , bx , by );
            
            double dx = bx - ax;
            double dy = by - ay;

            return sqrt( ( dx * dx ) + ( dy * dy ) );
        }

        double size( const GeoHash& a ) const {
            GeoHash b = a;
            b.move( 1 , 1 );
            return distance( a , b );
        }

        const IndexDetails* getDetails() const {
            return _spec->getDetails();
        }

        virtual auto_ptr<Cursor> newCursor( const BSONObj& query , const BSONObj& order , int numWanted ) const;

        virtual IndexSuitability suitability( const BSONObj& query , const BSONObj& order ) const {
            BSONElement e = query.getFieldDotted(_geo.c_str());
            switch ( e.type() ){
            case Object: {
                BSONObj sub = e.embeddedObject();
                switch ( sub.firstElement().getGtLtOp() ){
                case BSONObj::opNEAR:
                case BSONObj::opWITHIN:
                    return OPTIMAL;
                default:;
                }
            }
            case Array:
                return HELPFUL;
            default:
                return USELESS;
            }
        }

        string _geo;
        vector<string> _other;
        
        unsigned _bits;
        int _max;
        int _min;
        double _scaling;

        BSONObj _order;
    };

    class Point {
    public:
        
        Point( const Geo2dType * g , const GeoHash& hash ){
            g->_unconvert( hash , _x , _y );
        }

        Point( double x , double y )
            : _x( x ) , _y( y ){
        }
        
        Point() : _x(0),_y(0){
        }

        GeoHash hash( const Geo2dType * g ){
            return g->_hash( _x , _y );
        }
        
        string toString() const {
            StringBuilder buf(32);
            buf << "(" << _x << "," << _y << ")";
            return buf.str();
  
        }

        double _x;
        double _y;
    };

    class Box {
    public:
        
        Box( const Geo2dType * g , const GeoHash& hash )
            : _min( g , hash ) , 
              _max( _min._x + g->size( hash ) , _min._y + g->size( hash ) ){
        }
        
        Box( double x , double y , double size )
            : _min( x , y ) , 
              _max( x + size , y + size ){
        }

        Box( Point min , Point max )
            : _min( min ) , _max( max ){
        }

        Box(){}

        string toString() const {
            StringBuilder buf(64);
            buf << _min.toString() << " -->> " << _max.toString();
            return buf.str();
        }
        
        operator string() const {
            return toString();
        }

        bool between( double min , double max , double val , double fudge=0) const {
            return val + fudge >= min && val <= max + fudge;
        }
        
        bool mid( double amin , double amax , double bmin , double bmax , bool min , double& res ) const {
            assert( amin < amax );
            assert( bmin < bmax );

            if ( amin < bmin ){
                if ( amax < bmin )
                    return false;
                res = min ? bmin : amax;
                return true;
            }
            if ( amin > bmax )
                return false;
            res = min ? amin : bmax;
            return true;
        }

        double intersects( const Box& other ) const {
            
            Point boundMin(0,0);
            Point boundMax(0,0);
            
            if ( mid( _min._x , _max._x , other._min._x , other._max._x , true , boundMin._x ) == false ||
                 mid( _min._x , _max._x , other._min._x , other._max._x , false , boundMax._x ) == false ||
                 mid( _min._y , _max._y , other._min._y , other._max._y , true , boundMin._y ) == false ||
                 mid( _min._y , _max._y , other._min._y , other._max._y , false , boundMax._y ) == false )
                return 0;
            
            Box intersection( boundMin , boundMax );

            return intersection.area() / ( ( area() + other.area() ) / 2 );
        }

        double area() const {
            return ( _max._x - _min._x ) * ( _max._y - _min._y );
        }

        Point center() const {
            return Point( ( _min._x + _max._x ) / 2 ,
                          ( _min._y + _max._y ) / 2 );
        }

        bool inside( Point p , double fudge = 0 ){
            bool res = inside( p._x , p._y , fudge );
            //cout << "is : " << p.toString() << " in " << toString() << " = " << res << endl;
            return res;
        }
        
        bool inside( double x , double y , double fudge = 0 ){
            return 
                between( _min._x , _max._x  , x , fudge ) &&
                between( _min._y , _max._y  , y , fudge );
        }
        
        Point _min;
        Point _max;
    };
    
    class Geo2dPlugin : public IndexPlugin {
    public:
        Geo2dPlugin() : IndexPlugin( GEO2DNAME ){
        }
        
        virtual IndexType* generate( const IndexSpec* spec ) const {
            return new Geo2dType( this , spec );
        }
    } geo2dplugin;
    
    struct GeoUnitTest : public UnitTest {
        
        int round( double d ){
            return (int)(.5+(d*1000));
        }
        
#define GEOHEQ(a,b) if ( a.toString() != b ){ cout << "[" << a.toString() << "] != [" << b << "]" << endl; assert( a == b ); }

        void run(){
            assert( ! GeoHash::isBitSet( 0 , 0 ) );
            assert( ! GeoHash::isBitSet( 0 , 31 ) );
            assert( GeoHash::isBitSet( 1 , 31 ) );
            
            IndexSpec i( BSON( "loc" << "2d" ) );
            Geo2dType g( &geo2dplugin , &i );
            {
                double x = 73.01212;
                double y = 41.352964;
                BSONObj in = BSON( "x" << x << "y" << y );
                GeoHash h = g._hash( in );
                BSONObj out = g._unhash( h );
                assert( round(x) == round( out["x"].number() ) );
                assert( round(y) == round( out["y"].number() ) );
                assert( round( in["x"].number() ) == round( out["x"].number() ) );
                assert( round( in["y"].number() ) == round( out["y"].number() ) );
            }

            {
                double x = -73.01212;
                double y = 41.352964;
                BSONObj in = BSON( "x" << x << "y" << y );
                GeoHash h = g._hash( in );
                BSONObj out = g._unhash( h );
                assert( round(x) == round( out["x"].number() ) );
                assert( round(y) == round( out["y"].number() ) );
                assert( round( in["x"].number() ) == round( out["x"].number() ) );
                assert( round( in["y"].number() ) == round( out["y"].number() ) );
            }
            
            {
                GeoHash h( "0000" );
                h.move( 0 , 1 );
                GEOHEQ( h , "0001" );
                h.move( 0 , -1 );
                GEOHEQ( h , "0000" );

                h.init( "0001" );
                h.move( 0 , 1 );
                GEOHEQ( h , "0100" );
                h.move( 0 , -1 );
                GEOHEQ( h , "0001" );
                

                h.init( "0000" );
                h.move( 1 , 0 );
                GEOHEQ( h , "0010" );
            }
            
            {
                Box b( 5 , 5 , 2 );
                assert( "(5,5) -->> (7,7)" == b.toString() );
            }

            {
                GeoHash a = g._hash( 1 , 1 );
                GeoHash b = g._hash( 4 , 5 );
                assert( 5 == (int)(g.distance( a , b ) ) );
                a = g._hash( 50 , 50 );
                b = g._hash( 42 , 44 );
                assert( round(10) == round(g.distance( a , b )) );
            }
            
            {
                GeoHash x("0000");
                assert( 0 == x.getHash() );
                x.init( 0 , 1 , 32 );
                GEOHEQ( x , "0000000000000000000000000000000000000000000000000000000000000001" )

                assert( GeoHash( "1100").hasPrefix( GeoHash( "11" ) ) );
                assert( ! GeoHash( "1000").hasPrefix( GeoHash( "11" ) ) );
            }
               
            {
                GeoHash x("1010");
                GEOHEQ( x , "1010" );
                GeoHash y = x + "01";
                GEOHEQ( y , "101001" );
            }

            { 
                
                GeoHash a = g._hash( 5 , 5 );
                GeoHash b = g._hash( 5 , 7 );
                GeoHash c = g._hash( 100 , 100 );
                /*
                cout << "a: " << a << endl;
                cout << "b: " << b << endl;
                cout << "c: " << c << endl;

                cout << "a: " << a.toStringHex1() << endl;
                cout << "b: " << b.toStringHex1() << endl;
                cout << "c: " << c.toStringHex1() << endl;
                */
                BSONObj oa = a.wrap();
                BSONObj ob = b.wrap();
                BSONObj oc = c.wrap();
                /*
                cout << "a: " << oa.hexDump() << endl;
                cout << "b: " << ob.hexDump() << endl;
                cout << "c: " << oc.hexDump() << endl;
                */
                assert( oa.woCompare( ob ) < 0 );
                assert( oa.woCompare( oc ) < 0 );

            }

            {
                GeoHash x( "000000" );
                x.move( -1 , 0 );
                GEOHEQ( x , "101010" );
                x.move( 1 , -1 );
                GEOHEQ( x , "010101" );
                x.move( 0 , 1 );
                GEOHEQ( x , "000000" );
            }

            {
                GeoHash prefix( "110011000000" );
                GeoHash entry(  "1100110000011100000111000001110000011100000111000001000000000000" );
                assert( ! entry.hasPrefix( prefix ) );

                entry = "1100110000001100000111000001110000011100000111000001000000000000";
                assert( entry.toString().find( prefix.toString() ) == 0 );
                assert( entry.hasPrefix( GeoHash( "1100" ) ) );
                assert( entry.hasPrefix( prefix ) );
            }
            
            {
                GeoHash a = g._hash( 50 , 50 );
                GeoHash b = g._hash( 48 , 54 );
                assert( round( 4.47214 ) == round( g.distance( a , b ) ) );
            }
            

            {
                Box b( Point( 29.762283 , -95.364271 ) , Point( 29.764283000000002 , -95.36227099999999 ) );
                assert( b.inside( 29.763 , -95.363 ) );
                assert( ! b.inside( 32.9570255 , -96.1082497 ) );
                assert( ! b.inside( 32.9570255 , -96.1082497 , .01 ) );
            }

            {
                GeoHash a( "11001111" );
                assert( GeoHash( "11" ) == a.commonPrefix( "11" ) );
                assert( GeoHash( "11" ) == a.commonPrefix( "11110000" ) );
            }
            
        }
    } geoUnitTest;
    
    class GeoPoint {
    public:
        GeoPoint(){
        }

        GeoPoint( const KeyNode& node , double distance )
            : _key( node.key ) , _loc( node.recordLoc ) , _o( node.recordLoc.obj() ) , _distance( distance ){
        }

        GeoPoint( const BSONObj& key , DiskLoc loc , double distance )
            : _key(key) , _loc(loc) , _o( loc.obj() ) , _distance( distance ){
        }

        bool operator<( const GeoPoint& other ) const {
            return _distance < other._distance;
        }

        bool isEmpty() const {
            return _o.isEmpty();
        }

        BSONObj _key;
        DiskLoc _loc;
        BSONObj _o;
        double _distance;
    };

    class GeoAccumulator {
    public:
        GeoAccumulator( const Geo2dType * g , const BSONObj& filter )
            : _g(g) , _lookedAt(0) , _objectsLoaded(0) , _found(0) {
            if ( ! filter.isEmpty() ){
                _matcher.reset( new CoveredIndexMatcher( filter , g->keyPattern() ) );
            }
        }

        virtual ~GeoAccumulator(){
        }

        virtual void add( const KeyNode& node ){
            // when looking at other boxes, don't want to look at some object twice
            if ( _seen.count( node.recordLoc ) ){
                GEODEBUG( "\t\t\t\t already seen : " << node.recordLoc.obj()["_id"] );
                return;
            }
            _seen.insert( node.recordLoc );
            _lookedAt++;
            
            // distance check
            double d = 0;
            if ( ! checkDistance( GeoHash( node.key.firstElement() ) , d ) ){
                GEODEBUG( "\t\t\t\t bad distance : " << node.recordLoc.obj()  << "\t" << d );
                return;
            }
            
            // matcher
            MatchDetails details;
            if ( _matcher.get() ){
                bool good = _matcher->matches( node.key , node.recordLoc , &details );
                if ( details.loadedObject )
                    _objectsLoaded++;
                
                if ( ! good ){
                    GEODEBUG( "\t\t\t\t didn't match : " << node.recordLoc.obj()["_id"] );
                    return;
                }
            }
            
            if ( ! details.loadedObject ) // dont double count
                _objectsLoaded++;

            addSpecific( node , d );
            _found++;
        }

        virtual void addSpecific( const KeyNode& node , double d ) = 0;
        virtual bool checkDistance( const GeoHash& node , double& d ) = 0;

        long long found() const {
            return _found;
        }
        
        const Geo2dType * _g;
        set<DiskLoc> _seen;
        auto_ptr<CoveredIndexMatcher> _matcher;

        long long _lookedAt;
        long long _objectsLoaded;
        long long _found;
    };
    
    class GeoHopper : public GeoAccumulator {
    public:
        typedef multiset<GeoPoint> Holder;

        GeoHopper( const Geo2dType * g , unsigned max , const GeoHash& n , const BSONObj& filter = BSONObj() , double maxDistance = numeric_limits<double>::max() )
            : GeoAccumulator( g , filter ) , _max( max ) , _near( n ), _maxDistance( maxDistance ) {

        }

        virtual bool checkDistance( const GeoHash& h , double& d ){
            d = _g->distance( _near , h );
            bool good = d < _maxDistance && ( _points.size() < _max || d < farthest() );
            GEODEBUG( "\t\t\t\t\t\t\t checkDistance " << _near << "\t" << h << "\t" << d 
                      << " ok: " << good << " farthest: " << farthest() );
            return good;
        }
        
        virtual void addSpecific( const KeyNode& node , double d ){
            GEODEBUG( "\t\t" << GeoHash( node.key.firstElement() ) << "\t" << node.recordLoc.obj() << "\t" << d );
            _points.insert( GeoPoint( node.key , node.recordLoc , d ) );
            if ( _points.size() > _max ){
                _points.erase( --_points.end() );
            }
        }

        double farthest(){
            if ( _points.size() == 0 )
                return -1;
                
            Holder::iterator i = _points.end();
            i--;
            return i->_distance;
        }

        unsigned _max;
        GeoHash _near;
        Holder _points;
        double _maxDistance;

    };

    struct BtreeLocation {
        int pos;
        bool found;
        DiskLoc bucket;
        
        BSONObj key(){
            if ( bucket.isNull() )
                return BSONObj();
            return bucket.btree()->keyNode( pos ).key;
        }
        
        bool hasPrefix( const GeoHash& hash ){
            BSONElement e = key().firstElement();
            if ( e.eoo() )
                return false;
            return GeoHash( e ).hasPrefix( hash );
        }
        
        bool advance( int direction , int& totalFound , GeoAccumulator* all ){

            if ( bucket.isNull() )
                return false;
            bucket = bucket.btree()->advance( bucket , pos , direction , "btreelocation" );
            
            return checkCur( totalFound , all );
        }

        bool checkCur( int& totalFound , GeoAccumulator* all ){
            if ( bucket.isNull() )
                return false;

            if ( bucket.btree()->isUsed(pos) ){
                totalFound++;
                all->add( bucket.btree()->keyNode( pos ) );
            }
            else {
                GEODEBUG( "\t\t\t\t not used: " << key() );
            }

            return true;
        }

        string toString(){
            stringstream ss;
            ss << "bucket: " << bucket.toString() << " pos: " << pos << " found: " << found;
            return ss.str();
        }

        static bool initial( const IndexDetails& id , const Geo2dType * spec , 
                             BtreeLocation& min , BtreeLocation&  max , 
                             GeoHash start ,
                             int & found , GeoAccumulator * hopper ){
            
            min.bucket = id.head.btree()->locate( id , id.head , start.wrap() , 
                                                  spec->_order , min.pos , min.found , minDiskLoc );
            min.checkCur( found , hopper );
            max = min;
            
            if ( min.bucket.isNull() ){
                min.bucket = id.head.btree()->locate( id , id.head , start.wrap() , 
                                                      spec->_order , min.pos , min.found , minDiskLoc , -1 );
                min.checkCur( found , hopper );
            }
            
            return ! min.bucket.isNull() || ! max.bucket.isNull();
        }
    };

    class GeoSearch {
    public:
        GeoSearch( const Geo2dType * g , const GeoHash& n , int numWanted=100 , BSONObj filter=BSONObj() , double maxDistance = numeric_limits<double>::max() )
            : _spec( g ) , _n( n ) , _start( n ) ,
              _numWanted( numWanted ) , _filter( filter ) , _maxDistance( maxDistance ) ,
              _hopper( new GeoHopper( g , numWanted , n , filter , maxDistance ) )
        {
            assert( g->getDetails() );
            _nscanned = 0;
            _found = 0;
        }
        
        void exec(){
            const IndexDetails& id = *_spec->getDetails();
            
            BtreeBucket * head = id.head.btree();
            assert( head );
            /*
             * Search algorithm
             * 1) use geohash prefix to find X items
             * 2) compute max distance from want to an item
             * 3) find optimal set of boxes that complete circle
             * 4) use regular btree cursors to scan those boxes
             */
            
            GeoHopper * hopper = _hopper.get();

            _prefix = _start;
            { // 1 regular geo hash algorithm
                

                BtreeLocation min,max;
                if ( ! BtreeLocation::initial( id , _spec , min , max , _n , _found , hopper ) )
                    return;
                
                while ( _hopper->found() < _numWanted ){
                    GEODEBUG( _prefix << "\t" << _found << "\t DESC" );
                    while ( min.hasPrefix( _prefix ) && min.advance( -1 , _found , hopper ) )
                        _nscanned++;
                    GEODEBUG( _prefix << "\t" << _found << "\t ASC" );
                    while ( max.hasPrefix( _prefix ) && max.advance( 1 , _found , hopper ) )
                        _nscanned++;
                    if ( ! _prefix.constrains() )
                        break;
                    _prefix = _prefix.up();
                    
                    double temp = _spec->distance( _prefix , _start );
                    if ( temp > ( _maxDistance * 2 ) )
                        break;
                }
            }
            GEODEBUG( "done part 1" );
            if ( _found && _prefix.constrains() ){
                // 2
                Point center( _spec , _n );
                double boxSize = _spec->size( _prefix );
                double farthest = hopper->farthest();
                if ( farthest > boxSize )
                    boxSize = farthest;
                Box want( center._x - ( boxSize / 2 ) , center._y - ( boxSize / 2 ) , boxSize );
                while ( _spec->size( _prefix ) < boxSize )
                    _prefix = _prefix.up();
                log(1) << "want: " << want << " found:" << _found << " hash size:" << _spec->size( _prefix ) << endl;
                
                for ( int x=-1; x<=1; x++ ){
                    for ( int y=-1; y<=1; y++ ){
                        GeoHash toscan = _prefix;
                        toscan.move( x , y );
                        
                        // 3 & 4
                        doBox( id , want , toscan );
                    }
                }
            }
            GEODEBUG( "done search" )
            
        }

        void doBox( const IndexDetails& id , const Box& want , const GeoHash& toscan , int depth = 0 ){
            Box testBox( _spec , toscan );
            if ( logLevel > 0 ) log(1) << "\t doBox: " << testBox << "\t" << toscan.toString() << endl;

            double intPer = testBox.intersects( want );

            if ( intPer <= 0 )
                return;
            
            if ( intPer < .5 && depth < 3 ){
                doBox( id , want , toscan + "00" , depth + 1);
                doBox( id , want , toscan + "01" , depth + 1);
                doBox( id , want , toscan + "10" , depth + 1);
                doBox( id , want , toscan + "11" , depth + 1);
                return;
            }

            BtreeLocation loc;
            loc.bucket = id.head.btree()->locate( id , id.head , toscan.wrap() , _spec->_order , 
                                                        loc.pos , loc.found , minDiskLoc );
            loc.checkCur( _found , _hopper.get() );
            while ( loc.hasPrefix( toscan ) && loc.advance( 1 , _found , _hopper.get() ) )
                _nscanned++;

        }


        const Geo2dType * _spec;

        GeoHash _n;
        GeoHash _start;
        GeoHash _prefix;
        int _numWanted;
        BSONObj _filter;
        double _maxDistance;
        shared_ptr<GeoHopper> _hopper;

        long long _nscanned;
        int _found;
    };

    class GeoCursorBase : public Cursor {
    public:
        GeoCursorBase( const Geo2dType * spec )
            : _spec( spec ), _id( _spec->getDetails() ){

        }

        virtual DiskLoc refLoc(){ return DiskLoc(); }

        virtual BSONObj indexKeyPattern() {
            return _spec->keyPattern();
        }

        virtual void noteLocation() { 
            assert(0);
        }

        /* called before query getmore block is iterated */
        virtual void checkLocation() {
            assert(0);
        }

        virtual bool supportGetMore() { return false; }

        virtual bool getsetdup(DiskLoc loc){
            return false;
        }

        const Geo2dType * _spec;
        const IndexDetails * _id;
    };

    class GeoSearchCursor : public GeoCursorBase {
    public:
        GeoSearchCursor( shared_ptr<GeoSearch> s )
            : GeoCursorBase( s->_spec ) , 
              _s( s ) , _cur( s->_hopper->_points.begin() ) , _end( s->_hopper->_points.end() ) {
        }
        
        virtual ~GeoSearchCursor() {}
        
        virtual bool ok(){
            return _cur != _end;
        }
        
        virtual Record* _current(){ assert(ok()); return _cur->_loc.rec(); }
        virtual BSONObj current(){ assert(ok()); return _cur->_o; }
        virtual DiskLoc currLoc(){ assert(ok()); return _cur->_loc; }
        virtual bool advance(){ _cur++; return ok(); }
        virtual BSONObj currKey() const { return _cur->_key; }

        virtual string toString() {
            return "GeoSearchCursor";
        }


        virtual BSONObj prettyStartKey() const { 
            return BSON( _s->_spec->_geo << _s->_prefix.toString() ); 
        }
        virtual BSONObj prettyEndKey() const { 
            GeoHash temp = _s->_prefix;
            temp.move( 1 , 1 );
            return BSON( _s->_spec->_geo << temp.toString() ); 
        }


        shared_ptr<GeoSearch> _s;
        GeoHopper::Holder::iterator _cur;
        GeoHopper::Holder::iterator _end;
    };

    class GeoBrowse : public GeoCursorBase , public GeoAccumulator {
    public:
        GeoBrowse( const Geo2dType * g , string type , BSONObj filter = BSONObj() )
            : GeoCursorBase( g ) ,GeoAccumulator( g , filter ) ,
              _type( type ) , _filter( filter ) , _firstCall(true) {
        }
        
        virtual string toString() {
            return (string)"GeoBrowse-" + _type;
        }

        virtual bool ok(){
            if ( _firstCall ){
                fillStack();
                _firstCall = false;
            }
            if ( ! _cur.isEmpty() || _stack.size() )
                return true;

            while ( moreToDo() ){
                fillStack();
                if ( ! _cur.isEmpty() )
                    return true;
            }
            
            return false;
        }
        
        virtual bool advance(){ 
            _cur._o = BSONObj();
            
            if ( _stack.size() ){
                _cur = _stack.front();
                _stack.pop_front();
                return true;
            }
            
            if ( ! moreToDo() )
                return false;
            
            while ( _cur.isEmpty() && moreToDo() )
                fillStack();
            return ! _cur.isEmpty();
        }
        
        virtual Record* _current(){ assert(ok()); return _cur._loc.rec(); }
        virtual BSONObj current(){ assert(ok()); return _cur._o; }
        virtual DiskLoc currLoc(){ assert(ok()); return _cur._loc; }
        virtual BSONObj currKey() const { return _cur._key; }


        virtual bool moreToDo() = 0;
        virtual void fillStack() = 0;

        virtual void addSpecific( const KeyNode& node , double d ){
            if ( _cur.isEmpty() )
                _cur = GeoPoint( node , d );
            else
                _stack.push_back( GeoPoint( node , d ) );
        }

        string _type;
        BSONObj _filter;
        list<GeoPoint> _stack;

        GeoPoint _cur;
        bool _firstCall;

    };

    class GeoCircleBrowse : public GeoBrowse {
    public:
        
        enum State {
            START , 
            DOING_EXPAND ,
            DOING_AROUND ,
            DONE
        } _state;

        GeoCircleBrowse( const Geo2dType * g , const BSONObj& circle , BSONObj filter = BSONObj() )        
            : GeoBrowse( g , "circle" , filter ){
            
            uassert( 13060 , "$center needs 2 fields (middle,max distance)" , circle.nFields() == 2 );
            BSONObjIterator i(circle);
            _start = g->_tohash( i.next() );
            _prefix = _start;
            _maxDistance = i.next().numberDouble();
            uassert( 13061 , "need a max distance > 0 " , _maxDistance > 0 );

            _state = START;
            _found = 0;

            ok();
        }

        virtual bool moreToDo(){
            return _state != DONE;
        }
        
        virtual void fillStack(){
            if ( _state == START ){
                if ( ! BtreeLocation::initial( *_id , _spec , _min , _max , 
                                               _prefix , _found , this ) ){
                    _state = DONE;
                    return;
                }
                _state = DOING_EXPAND;
            }
            
            if ( _state == DOING_EXPAND ){
                GEODEBUG( "circle prefix [" << _prefix << "]" );
                while ( _min.hasPrefix( _prefix ) && _min.advance( -1 , _found , this ) );
                while ( _max.hasPrefix( _prefix ) && _max.advance( 1 , _found , this ) );
                
                if ( ! _prefix.constrains() ){
                    GEODEBUG( "\t exhausted the btree" );
                    _state = DONE;
                    return;
                }
                
                if ( _g->distance( _prefix , _start ) > _maxDistance ){
                    GEODEBUG( "\tpast circle bounds" );
                    GeoHash tr = _prefix;
                    tr.move( 1 , 1 );
                    if ( _g->distance( tr , _start ) > _maxDistance )
                        _state = DOING_AROUND;
                    else
                        _prefix = _prefix.up();
                }
                else
                    _prefix = _prefix.up();
                return;
            }
            
            if ( _state == DOING_AROUND ){
                _state = DONE;
                return;
            }
        }
        
        virtual bool checkDistance( const GeoHash& h , double& d ){
            d = _g->distance( _start , h );
            GEODEBUG( "\t " << h << "\t" << d );
            return d <= ( _maxDistance + .01 );
        }

        GeoHash _start;
        double _maxDistance;
        
        int _found;
        
        GeoHash _prefix;        
        BtreeLocation _min;
        BtreeLocation _max;

    };    

    class GeoBoxBrowse : public GeoBrowse {
    public:
        
        enum State {
            START , 
            DOING_EXPAND ,
            DONE
        } _state;

        GeoBoxBrowse( const Geo2dType * g , const BSONObj& box , BSONObj filter = BSONObj() )        
            : GeoBrowse( g , "box" , filter ){
            
            uassert( 13063 , "$box needs 2 fields (bottomLeft,topRight)" , box.nFields() == 2 );
            BSONObjIterator i(box);
            _bl = g->_tohash( i.next() );
            _tr = g->_tohash( i.next() );

            _want._min = Point( _g , _bl );
            _want._max = Point( _g , _tr );
            
            uassert( 13064 , "need an area > 0 " , _want.area() > 0 );

            _state = START;
            _found = 0;

            Point center = _want.center();
            _prefix = _g->_hash( center._x , center._y );
            
            GEODEBUG( "center : " << center.toString() << "\t" << _prefix );

	    {
	      GeoHash a(0LL,32);
	      GeoHash b(0LL,32);
	      b.move(1,1);
	      _fudge = _g->distance(a,b);
	    }

            ok();
        }

        virtual bool moreToDo(){
            return _state != DONE;
        }
        
        virtual void fillStack(){
            if ( _state == START ){

                if ( ! BtreeLocation::initial( *_id , _spec , _min , _max , 
                                               _prefix , _found , this ) ){
                    _state = DONE;
                    return;
                }
                _state = DOING_EXPAND;
            }
            
            if ( _state == DOING_EXPAND ){
                int started = _found;
                while ( started == _found || _state == DONE ){
                    GEODEBUG( "box prefix [" << _prefix << "]" );
                    while ( _min.hasPrefix( _prefix ) && _min.advance( -1 , _found , this ) );
                    while ( _max.hasPrefix( _prefix ) && _max.advance( 1 , _found , this ) );
                    
                    if ( _state == DONE )
                        return;

                    if ( ! _prefix.constrains() ){
                        GEODEBUG( "box exhausted" );
                        _state = DONE;
                        return;
                    }
                    
                    Box cur( _g , _prefix );
                    if ( cur._min._x + _fudge < _want._min._x &&
                         cur._min._y + _fudge < _want._min._y &&
                         cur._max._x - _fudge > _want._max._x &&
                         cur._max._y - _fudge > _want._max._y ){
                        
                        _state = DONE;
                        GeoHash temp = _prefix.commonPrefix( cur._max.hash( _g ) );

                        GEODEBUG( "box done : " << cur.toString() << " prefix:" << _prefix << " common:" << temp );
                        
                        if ( temp == _prefix )
                            return;
                        _prefix = temp;
                        GEODEBUG( "\t one more loop" );
                        continue;
                    }
                    else {
                        _prefix = _prefix.up();
                    }
                }
                return;
            }

        }
        
        virtual bool checkDistance( const GeoHash& h , double& d ){
            bool res = _want.inside( Point( _g , h ) , _fudge );
            GEODEBUG( "\t want : " << _want.toString() 
                      << " point: " << Point( _g , h ).toString() 
                      << " in : " << res );
            return res;
        }

        GeoHash _bl;
        GeoHash _tr;
        Box _want;

        int _found;
        
        GeoHash _prefix;        
        BtreeLocation _min;
        BtreeLocation _max;

        double _fudge;
    };    


    auto_ptr<Cursor> Geo2dType::newCursor( const BSONObj& query , const BSONObj& order , int numWanted ) const {
        if ( numWanted < 0 )
            numWanted = numWanted * -1;
        else if ( numWanted == 0 )
             numWanted = 100;
        
        BSONObjIterator i(query);
        while ( i.more() ){
            BSONElement e = i.next();

            if ( _geo != e.fieldName() )
                continue;

            if ( e.type() != Object )
                continue;
            
            switch ( e.embeddedObject().firstElement().getGtLtOp() ){
            case BSONObj::opNEAR: {
                e = e.embeddedObject().firstElement();
                double maxDistance = numeric_limits<double>::max();
                if ( e.isABSONObj() && e.embeddedObject().nFields() > 2 ){
                    BSONObjIterator i(e.embeddedObject());
                    i.next();
                    i.next();
                    BSONElement e = i.next();
                    if ( e.isNumber() )
                        maxDistance = e.numberDouble();
                }
                shared_ptr<GeoSearch> s( new GeoSearch( this , _tohash(e) , numWanted , query , maxDistance ) );
                s->exec();
                auto_ptr<Cursor> c;
                c.reset( new GeoSearchCursor( s ) );
                return c;   
            }
            case BSONObj::opWITHIN: {
                e = e.embeddedObject().firstElement();
                uassert( 13057 , "$within has to take an object or array" , e.isABSONObj() );
                e = e.embeddedObject().firstElement();
                string type = e.fieldName();
                if ( type == "$center" ){
                    uassert( 13059 , "$center has to take an object or array" , e.isABSONObj() );
                    auto_ptr<Cursor> c;
                    c.reset( new GeoCircleBrowse( this , e.embeddedObjectUserCheck() , query ) );
                    return c;   
                }
                else if ( type == "$box" ){
                    uassert( 13065 , "$box has to take an object or array" , e.isABSONObj() );
                    auto_ptr<Cursor> c;
                    c.reset( new GeoBoxBrowse( this , e.embeddedObjectUserCheck() , query ) );
                    return c;   
                }
                throw UserException( 13058 , (string)"unknown $with type: " + type );
            }
            default: 
                break;
            }
        }

        throw UserException( 13042 , (string)"missing geo field (" + _geo + ") in : " + query.toString() );
    }

    // ------
    // commands
    // ------

    class Geo2dFindNearCmd : public Command {
    public:
        Geo2dFindNearCmd() : Command( "geoNear" ){}
        virtual LockType locktype(){ return READ; } 
        bool slaveOk() { return true; }
        bool slaveOverrideOk() { return true; }
        bool run(const char * stupidns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl){
            string ns = nsToDatabase( stupidns ) + "." + cmdObj.firstElement().valuestr();

            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( ! d ){
                errmsg = "can't find ns";
                return false;
            }

            int geoIdx = -1;
            {
                NamespaceDetails::IndexIterator ii = d->ii();
                while ( ii.more() ){
                    IndexDetails& id = ii.next();
                    if ( id.getSpec().getTypeName() == GEO2DNAME ){
                        if ( geoIdx >= 0 ){
                            errmsg = "2 geo indexes :(";
                            return false;
                        }
                        geoIdx = ii.pos() - 1;
                    }
                }
            }
            
            if ( geoIdx < 0 ){
                errmsg = "no geo index :(";
                return false;
            }
            
            result.append( "ns" , ns );

            IndexDetails& id = d->idx( geoIdx );
            Geo2dType * g = (Geo2dType*)id.getSpec().getType();
            assert( &id == g->getDetails() );
            
            int numWanted = 100;
            if ( cmdObj["num"].isNumber() )
                numWanted = cmdObj["num"].numberInt();

            uassert(13046, "'near' param missing/invalid", !cmdObj["near"].eoo());
            const GeoHash n = g->_tohash( cmdObj["near"] );
            result.append( "near" , n.toString() );

            BSONObj filter;
            if ( cmdObj["query"].type() == Object )
                filter = cmdObj["query"].embeddedObject();

            double maxDistance = numeric_limits<double>::max();
            if ( cmdObj["maxDistance"].isNumber() )
                maxDistance = cmdObj["maxDistance"].number();

            GeoSearch gs( g , n , numWanted , filter , maxDistance );

            if ( cmdObj["start"].type() == String){
                GeoHash start = (string) cmdObj["start"].valuestr();
                gs._start = start;
            }
            
            gs.exec();

            double distanceMultiplier = 1;
            if ( cmdObj["distanceMultiplier"].isNumber() )
                distanceMultiplier = cmdObj["distanceMultiplier"].number();
            
            double totalDistance = 0;


            BSONObjBuilder arr( result.subarrayStart( "results" ) );
            int x = 0;
            for ( GeoHopper::Holder::iterator i=gs._hopper->_points.begin(); i!=gs._hopper->_points.end(); i++ ){
                const GeoPoint& p = *i;
                
                double dis = distanceMultiplier * p._distance;
                totalDistance += dis;
                
                BSONObjBuilder bb( arr.subobjStart( BSONObjBuilder::numStr( x++ ).c_str() ) );
                bb.append( "dis" , dis );
                bb.append( "obj" , p._o );
                bb.done();
            }
            arr.done();
            
            BSONObjBuilder stats( result.subobjStart( "stats" ) );
            stats.append( "time" , cc().curop()->elapsedMillis() );
            stats.appendNumber( "btreelocs" , gs._nscanned );
            stats.appendNumber( "nscanned" , gs._hopper->_lookedAt );
            stats.appendNumber( "objectsLoaded" , gs._hopper->_objectsLoaded );
            stats.append( "avgDistance" , totalDistance / x );
            stats.done();
            
            return true;
        }
        
    } geo2dFindNearCmd;

    class GeoWalkCmd : public Command {
    public:
        GeoWalkCmd() : Command( "geoWalk" ){}
        virtual LockType locktype(){ return READ; } 
        bool slaveOk() { return true; }
        bool slaveOverrideOk() { return true; }
        bool run(const char * stupidns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl){
            string ns = nsToDatabase( stupidns ) + "." + cmdObj.firstElement().valuestr();

            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( ! d ){
                errmsg = "can't find ns";
                return false;
            }

            int geoIdx = -1;
            {
                NamespaceDetails::IndexIterator ii = d->ii();
                while ( ii.more() ){
                    IndexDetails& id = ii.next();
                    if ( id.getSpec().getTypeName() == GEO2DNAME ){
                        if ( geoIdx >= 0 ){
                            errmsg = "2 geo indexes :(";
                            return false;
                        }
                        geoIdx = ii.pos() - 1;
                    }
                }
            }
            
            if ( geoIdx < 0 ){
                errmsg = "no geo index :(";
                return false;
            }
            

            IndexDetails& id = d->idx( geoIdx );
            Geo2dType * g = (Geo2dType*)id.getSpec().getType();
            assert( &id == g->getDetails() );

            int max = 100000;

            BtreeCursor c( d , geoIdx , id , BSONObj() , BSONObj() , true , 1 );
            while ( c.ok() && max-- ){
                GeoHash h( c.currKey().firstElement() );
                int len;
                cout << "\t" << h.toString()
                     << "\t" << c.current()[g->_geo] 
                     << "\t" << hex << h.getHash() 
                     << "\t" << hex << ((long long*)c.currKey().firstElement().binData(len))[0]
                     << "\t" << c.current()["_id"]
                     << endl;
                c.advance();
            }

            return true;
        }
        
    } geoWalkCmd;

}
