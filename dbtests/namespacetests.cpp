// namespacetests.cpp : namespace.{h,cpp} unit tests.
//

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

// Where IndexDetails defined.
#include "stdafx.h"
#include "../db/namespace.h"

#include "../db/db.h"
#include "../db/json.h"

#include "dbtests.h"

namespace NamespaceTests {
    namespace IndexDetailsTests {
        class Base {
            dblock lk;
            Client::Context _context;
        public:
            Base() : _context(ns()){
            }
            virtual ~Base() {
                if ( id_.info.isNull() )
                    return;
                theDataFileMgr.deleteRecord( ns(), id_.info.rec(), id_.info );
                ASSERT( theDataFileMgr.findAll( ns() )->eof() );
            }
        protected:
            void create() {
                NamespaceDetailsTransient::get_w( ns() ).deletedIndex();
                BSONObjBuilder builder;
                builder.append( "ns", ns() );
                builder.append( "name", "testIndex" );
                builder.append( "key", key() );
                BSONObj bobj = builder.done();
                id_.info = theDataFileMgr.insert( ns(), bobj.objdata(), bobj.objsize() );
                // head not needed for current tests
                // idx_.head = BtreeBucket::addHead( id_ );
            }
            static const char* ns() {
                return "unittests.indexdetailstests";
            }
            IndexDetails& id() {
                return id_;
            }
            virtual BSONObj key() const {
                BSONObjBuilder k;
                k.append( "a", 1 );
                return k.obj();
            }
            BSONObj aDotB() const {
                BSONObjBuilder k;
                k.append( "a.b", 1 );
                return k.obj();
            }
            BSONObj aAndB() const {
                BSONObjBuilder k;
                k.append( "a", 1 );
                k.append( "b", 1 );
                return k.obj();
            }
            static vector< int > shortArray() {
                vector< int > a;
                a.push_back( 1 );
                a.push_back( 2 );
                a.push_back( 3 );
                return a;
            }
            static BSONObj simpleBC( int i ) {
                BSONObjBuilder b;
                b.append( "b", i );
                b.append( "c", 4 );
                return b.obj();
            }
            static void checkSize( int expected, const BSONObjSetDefaultOrder  &objs ) {
                ASSERT_EQUALS( BSONObjSetDefaultOrder::size_type( expected ), objs.size() );
            }
            static void assertEquals( const BSONObj &a, const BSONObj &b ) {
                if ( a.woCompare( b ) != 0 ) {
                    out() << "expected: " << a.toString()
                          << ", got: " << b.toString() << endl;
                }
                ASSERT( a.woCompare( b ) == 0 );
            }
            BSONObj nullObj() const {
                BSONObjBuilder b;
                b.appendNull( "" );
                return b.obj();
            }
        private:
            dblock lk_;
            IndexDetails id_;
        };

        class Create : public Base {
        public:
            void run() {
                create();
                ASSERT_EQUALS( "testIndex", id().indexName() );
                ASSERT_EQUALS( ns(), id().parentNS() );
                assertEquals( key(), id().keyPattern() );
            }
        };

        class GetKeysFromObjectSimple : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b, e;
                b.append( "b", 4 );
                b.append( "a", 5 );
                e.append( "", 5 );
                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 1, keys );
                assertEquals( e.obj(), *keys.begin() );
            }
        };

        class GetKeysFromObjectDotted : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder a, e, b;
                b.append( "b", 4 );
                a.append( "a", b.done() );
                a.append( "c", "foo" );
                e.append( "", 4 );
                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( a.done(), keys );
                checkSize( 1, keys );
                ASSERT_EQUALS( e.obj(), *keys.begin() );
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class GetKeysFromArraySimple : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b;
                b.append( "a", shortArray()) ;

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.obj(), *i );
                }
            }
        };

        class GetKeysFromArrayFirstElement : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b;
                b.append( "a", shortArray() );
                b.append( "b", 2 );

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    b.append( "", 2 );
                    assertEquals( b.obj(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                return aAndB();
            }
        };

        class GetKeysFromArraySecondElement : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b;
                b.append( "first", 5 );
                b.append( "a", shortArray()) ;

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", 5 );
                    b.append( "", j );
                    assertEquals( b.obj(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                BSONObjBuilder k;
                k.append( "first", 1 );
                k.append( "a", 1 );
                return k.obj();
            }
        };

        class GetKeysFromSecondLevelArray : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b;
                b.append( "b", shortArray() );
                BSONObjBuilder a;
                a.append( "a", b.done() );

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( a.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.obj(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class ParallelArraysBasic : public Base {
        public:
            void run() {
                create();
                BSONObjBuilder b;
                b.append( "a", shortArray() );
                b.append( "b", shortArray() );

                BSONObjSetDefaultOrder keys;
                ASSERT_EXCEPTION( id().getKeysFromObject( b.done(), keys ),
                                  UserException );
            }
        private:
            virtual BSONObj key() const {
                return aAndB();
            }
        };

        class ArraySubobjectBasic : public Base {
        public:
            void run() {
                create();
                vector< BSONObj > elts;
                for ( int i = 1; i < 4; ++i )
                    elts.push_back( simpleBC( i ) );
                BSONObjBuilder b;
                b.append( "a", elts );

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.obj(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class ArraySubobjectMultiFieldIndex : public Base {
        public:
            void run() {
                create();
                vector< BSONObj > elts;
                for ( int i = 1; i < 4; ++i )
                    elts.push_back( simpleBC( i ) );
                BSONObjBuilder b;
                b.append( "a", elts );
                b.append( "d", 99 );

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 3, keys );
                int j = 1;
                for ( BSONObjSetDefaultOrder::iterator i = keys.begin(); i != keys.end(); ++i, ++j ) {
                    BSONObjBuilder c;
                    c.append( "", j );
                    c.append( "", 99 );
                    assertEquals( c.obj(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                BSONObjBuilder k;
                k.append( "a.b", 1 );
                k.append( "d", 1 );
                return k.obj();
            }
        };
        
        class ArraySubobjectSingleMissing : public Base {
        public:
            void run() {
                create();
                vector< BSONObj > elts;
                BSONObjBuilder s;
                s.append( "foo", 41 );
                elts.push_back( s.obj() );
                for ( int i = 1; i < 4; ++i )
                    elts.push_back( simpleBC( i ) );
                BSONObjBuilder b;
                b.append( "a", elts );
                
                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 4, keys );
                BSONObjSetDefaultOrder::iterator i = keys.begin();
                assertEquals( nullObj(), *i++ );
                for ( int j = 1; j < 4; ++i, ++j ) {
                    BSONObjBuilder b;
                    b.append( "", j );
                    assertEquals( b.obj(), *i );
                }
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };
        
        class ArraySubobjectMissing : public Base {
        public:
            void run() {
                create();
                vector< BSONObj > elts;
                BSONObjBuilder s;
                s.append( "foo", 41 );
                for ( int i = 1; i < 4; ++i )
                    elts.push_back( s.done() );
                BSONObjBuilder b;
                b.append( "a", elts );

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( b.done(), keys );
                checkSize( 1, keys );
                assertEquals( nullObj(), *keys.begin() );
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };
        
        class MissingField : public Base {
        public:
            void run() {
                create();
                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( BSON( "b" << 1 ), keys );
                checkSize( 1, keys );
                assertEquals( nullObj(), *keys.begin() );
            }
        private:
            virtual BSONObj key() const {
                return BSON( "a" << 1 );
            }
        };
        
        class SubobjectMissing : public Base {
        public:
            void run() {
                create();
                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( fromjson( "{a:[1,2]}" ), keys );
                checkSize( 1, keys );
                assertEquals( nullObj(), *keys.begin() );
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };
        
        class CompoundMissing : public Base {
        public:
            void run(){
                create();
                
                {
                    BSONObjSetDefaultOrder keys;
                    id().getKeysFromObject( fromjson( "{x:'a',y:'b'}" ) , keys );
                    checkSize( 1 , keys );
                    assertEquals( BSON( "" << "a" << "" << "b" ) , *keys.begin() );
                }

                {
                    BSONObjSetDefaultOrder keys;
                    id().getKeysFromObject( fromjson( "{x:'a'}" ) , keys );
                    checkSize( 1 , keys );
                    BSONObjBuilder b;
                    b.append( "" , "a" );
                    b.appendNull( "" );
                    assertEquals( b.obj() , *keys.begin() );
                }
                
            }

        private:
            virtual BSONObj key() const {
                return BSON( "x" << 1 << "y" << 1 );
            }
            
        };
        
        class ArraySubelementComplex : public Base {
        public:
            void run() {
                create();
                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( fromjson( "{a:[{b:[2]}]}" ), keys );
                checkSize( 1, keys );
                assertEquals( BSON( "" << 2 ), *keys.begin() );
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };

        class ParallelArraysComplex : public Base {
        public:
            void run() {
                create();
                BSONObjSetDefaultOrder keys;
                ASSERT_EXCEPTION( id().getKeysFromObject( fromjson( "{a:[{b:[1],c:[2]}]}" ), keys ),
                                  UserException );
            }
        private:
            virtual BSONObj key() const {
                return fromjson( "{'a.b':1,'a.c':1}" );
            }
        };

        class AlternateMissing : public Base {
        public:
            void run() {
                create();
                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( fromjson( "{a:[{b:1},{c:2}]}" ), keys );
                checkSize( 2, keys );
                BSONObjSetDefaultOrder::iterator i = keys.begin();
                {
                    BSONObjBuilder e;
                    e.appendNull( "" );
                    e.append( "", 2 );
                    assertEquals( e.obj(), *i++ );
                }

                {
                    BSONObjBuilder e;
                    e.append( "", 1 );
                    e.appendNull( "" );
                    assertEquals( e.obj(), *i++ );
                }
            }
        private:
            virtual BSONObj key() const {
                return fromjson( "{'a.b':1,'a.c':1}" );
            }
        };

        class MultiComplex : public Base {
        public:
            void run() {
                create();
                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( fromjson( "{a:[{b:1},{b:[1,2,3]}]}" ), keys );
                checkSize( 3, keys );
            }
        private:
            virtual BSONObj key() const {
                return aDotB();
            }
        };
        
        class EmptyArray : Base {
        public:
            void run(){
                create();

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( fromjson( "{a:[1,2]}" ), keys );
                checkSize(2, keys );
                keys.clear();
                
                id().getKeysFromObject( fromjson( "{a:[1]}" ), keys );
                checkSize(1, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:null}" ), keys );
                checkSize(1, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:[]}" ), keys );
                checkSize(1, keys );
                keys.clear();
            }
        };

        class MultiEmptyArray : Base {
        public:
            void run(){
                create();

                BSONObjSetDefaultOrder keys;
                id().getKeysFromObject( fromjson( "{a:1,b:[1,2]}" ), keys );
                checkSize(2, keys );
                keys.clear();
                
                id().getKeysFromObject( fromjson( "{a:1,b:[1]}" ), keys );
                checkSize(1, keys );
                keys.clear();

                id().getKeysFromObject( fromjson( "{a:1,b:null}" ), keys );
                cout << "YO : " << *(keys.begin()) << endl;
                checkSize(1, keys );
                keys.clear();
                
                id().getKeysFromObject( fromjson( "{a:1,b:[]}" ), keys );
                checkSize(1, keys );
                cout << "YO : " << *(keys.begin()) << endl;
                ASSERT_EQUALS( NumberInt , keys.begin()->firstElement().type() );
                keys.clear();
            }

        protected:
            BSONObj key() const {
                return aAndB();
            }
        };


    } // namespace IndexDetailsTests

    namespace NamespaceDetailsTests {

        class Base {
            const char *ns_;
            dblock lk;
            Client::Context _context;
        public:
            Base( const char *ns = "unittests.NamespaceDetailsTests" ) : ns_( ns ) , _context( ns ) {}
            virtual ~Base() {
                if ( !nsd() )
                    return;
                string s( ns() );
                string errmsg;
                BSONObjBuilder result;
                dropCollection( s, errmsg, result );
            }
        protected:
            void create() {
                dblock lk;
                string err;
                ASSERT( userCreateNS( ns(), fromjson( spec() ), err, false ) );
            }
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512}";
            }
            int nRecords() const {
                int count = 0;
                for ( DiskLoc i = nsd()->firstExtent; !i.isNull(); i = i.ext()->xnext ) {
                    int fileNo = i.ext()->firstRecord.a();
                    if ( fileNo == -1 )
                        continue;
                    for ( int j = i.ext()->firstRecord.getOfs(); j != DiskLoc::NullOfs;
                          j = DiskLoc( fileNo, j ).rec()->nextOfs ) {
                        ++count;
                    }
                }
                ASSERT_EQUALS( count, nsd()->nrecords );
                return count;
            }
            int nExtents() const {
                int count = 0;
                for ( DiskLoc i = nsd()->firstExtent; !i.isNull(); i = i.ext()->xnext )
                    ++count;
                return count;
            }
            static int min( int a, int b ) {
                return a < b ? a : b;
            }
            const char *ns() const {
                return ns_;
            }
            NamespaceDetails *nsd() const {
                return nsdetails( ns() );
            }
            static BSONObj bigObj() {
                string as( 187, 'a' );
                BSONObjBuilder b;
                b.append( "a", as );
                return b.obj();
            }
        };

        class Create : public Base {
        public:
            void run() {
                create();
                ASSERT( nsd() );
                ASSERT_EQUALS( 0, nRecords() );
                ASSERT( nsd()->firstExtent == nsd()->capExtent );
                DiskLoc initial = DiskLoc();
                initial.setInvalid();
                ASSERT( initial == nsd()->capFirstNewRecord );
            }
        };

        class SingleAlloc : public Base {
        public:
            void run() {
                create();
                BSONObj b = bigObj();
                ASSERT( !theDataFileMgr.insert( ns(), b.objdata(), b.objsize() ).isNull() );
                ASSERT_EQUALS( 1, nRecords() );
            }
        };

        class Realloc : public Base {
        public:
            void run() {
                create();
                BSONObj b = bigObj();

                DiskLoc l[ 6 ];
                for ( int i = 0; i < 6; ++i ) {
                    l[ i ] = theDataFileMgr.insert( ns(), b.objdata(), b.objsize() );
                    ASSERT( !l[ i ].isNull() );
                    ASSERT_EQUALS( 1 + i % 2, nRecords() );
                    if ( i > 1 )
                        ASSERT( l[ i ] == l[ i - 2 ] );
                }
            }
        };

        class TwoExtent : public Base {
        public:
            void run() {
                create();
                ASSERT_EQUALS( 2, nExtents() );
                BSONObj b = bigObj();

                DiskLoc l[ 8 ];
                for ( int i = 0; i < 8; ++i ) {
                    l[ i ] = theDataFileMgr.insert( ns(), b.objdata(), b.objsize() );
                    ASSERT( !l[ i ].isNull() );
                    ASSERT_EQUALS( i < 2 ? i + 1 : 3 + i % 2, nRecords() );
                    if ( i > 3 )
                        ASSERT( l[ i ] == l[ i - 4 ] );
                }

                // Too big
                BSONObjBuilder bob;
                bob.append( "a", string( 787, 'a' ) );
                BSONObj bigger = bob.done();
                ASSERT( theDataFileMgr.insert( ns(), bigger.objdata(), bigger.objsize() ).isNull() );
                ASSERT_EQUALS( 0, nRecords() );
            }
        private:
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":2}";
            }
        };

        class Migrate : public Base {
        public:
            void run() {
                create();
                nsd()->deletedList[ 2 ] = nsd()->deletedList[ 0 ].drec()->nextDeleted.drec()->nextDeleted;
                nsd()->deletedList[ 0 ].drec()->nextDeleted.drec()->nextDeleted = DiskLoc();
                nsd()->deletedList[ 1 ].Null();
                NamespaceDetails *d = nsd();
                zero( &d->capExtent );
                zero( &d->capFirstNewRecord );

                nsd();

                ASSERT( nsd()->firstExtent == nsd()->capExtent );
                ASSERT( nsd()->capExtent.getOfs() != 0 );
                ASSERT( !nsd()->capFirstNewRecord.isValid() );
                int nDeleted = 0;
                for ( DiskLoc i = nsd()->deletedList[ 0 ]; !i.isNull(); i = i.drec()->nextDeleted, ++nDeleted );
                ASSERT_EQUALS( 10, nDeleted );
                ASSERT( nsd()->deletedList[ 1 ].isNull() );
            }
        private:
            static void zero( DiskLoc *d ) {
                memset( d, 0, sizeof( DiskLoc ) );
            }
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":10}";
            }
        };

        // This isn't a particularly useful test, and because it doesn't clean up
        // after itself, /tmp/unittest needs to be cleared after running.
        //        class BigCollection : public Base {
        //        public:
        //            BigCollection() : Base( "NamespaceDetailsTests_BigCollection" ) {}
        //            void run() {
        //                create();
        //                ASSERT_EQUALS( 2, nExtents() );
        //            }
        //        private:
        //            virtual string spec() const {
        //                // NOTE 256 added to size in _userCreateNS()
        //                long long big = MongoDataFile::maxSize() - MDFHeader::headerSize();
        //                stringstream ss;
        //                ss << "{\"capped\":true,\"size\":" << big << "}";
        //                return ss.str();
        //            }
        //        };

        class Size {
        public:
            void run() {
                ASSERT_EQUALS( 496U, sizeof( NamespaceDetails ) );
            }
        };
        
    } // namespace NamespaceDetailsTests

    class All : public Suite {
    public:
        All() : Suite( "namespace" ){
        }

        void setupTests(){
            add< IndexDetailsTests::Create >();
            add< IndexDetailsTests::GetKeysFromObjectSimple >();
            add< IndexDetailsTests::GetKeysFromObjectDotted >();
            add< IndexDetailsTests::GetKeysFromArraySimple >();
            add< IndexDetailsTests::GetKeysFromArrayFirstElement >();
            add< IndexDetailsTests::GetKeysFromArraySecondElement >();
            add< IndexDetailsTests::GetKeysFromSecondLevelArray >();
            add< IndexDetailsTests::ParallelArraysBasic >();
            add< IndexDetailsTests::ArraySubobjectBasic >();
            add< IndexDetailsTests::ArraySubobjectMultiFieldIndex >();
            add< IndexDetailsTests::ArraySubobjectSingleMissing >();
            add< IndexDetailsTests::ArraySubobjectMissing >();
            add< IndexDetailsTests::ArraySubelementComplex >();
            add< IndexDetailsTests::ParallelArraysComplex >();
            add< IndexDetailsTests::AlternateMissing >();
            add< IndexDetailsTests::MultiComplex >();
            add< IndexDetailsTests::EmptyArray >();
            add< IndexDetailsTests::MultiEmptyArray >();
            add< IndexDetailsTests::MissingField >();
            add< IndexDetailsTests::SubobjectMissing >();
            add< IndexDetailsTests::CompoundMissing >();
            add< NamespaceDetailsTests::Create >();
            add< NamespaceDetailsTests::SingleAlloc >();
            add< NamespaceDetailsTests::Realloc >();
            add< NamespaceDetailsTests::TwoExtent >();
            add< NamespaceDetailsTests::Migrate >();
            //            add< NamespaceDetailsTests::BigCollection >();
            add< NamespaceDetailsTests::Size >();
        }
    } myall;
} // namespace NamespaceTests

