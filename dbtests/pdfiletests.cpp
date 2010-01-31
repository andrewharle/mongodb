// pdfiletests.cpp : pdfile unit tests.
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

#include "stdafx.h"
#include "../db/pdfile.h"

#include "../db/db.h"
#include "../db/json.h"

#include "dbtests.h"

namespace PdfileTests {

    namespace ScanCapped {

        class Base {
        public:
            Base() {
                setClient( ns() );
            }
            virtual ~Base() {
                if ( !nsd() )
                    return;
                string n( ns() );
                dropNS( n );
            }
            void run() {
                stringstream spec;
                spec << "{\"capped\":true,\"size\":2000,\"$nExtents\":" << nExtents() << "}";
                string err;
                ASSERT( userCreateNS( ns(), fromjson( spec.str() ), err, false ) );
                prepare();
                int j = 0;
                for ( auto_ptr< Cursor > i = theDataFileMgr.findAll( ns() );
                        i->ok(); i->advance(), ++j )
                    ASSERT_EQUALS( j, i->current().firstElement().number() );
                ASSERT_EQUALS( count(), j );

                j = count() - 1;
                for ( auto_ptr< Cursor > i =
                            findTableScan( ns(), fromjson( "{\"$natural\":-1}" ) );
                        i->ok(); i->advance(), --j )
                    ASSERT_EQUALS( j, i->current().firstElement().number() );
                ASSERT_EQUALS( -1, j );
            }
        protected:
            virtual void prepare() = 0;
            virtual int count() const = 0;
            virtual int nExtents() const {
                return 0;
            }
            // bypass standard alloc/insert routines to use the extent we want.
            static DiskLoc insert( DiskLoc ext, int i ) {
                BSONObjBuilder b;
                b.append( "a", i );
                BSONObj o = b.done();
                int len = o.objsize();
                Extent *e = ext.ext();
                int ofs;
                if ( e->lastRecord.isNull() )
                    ofs = ext.getOfs() + ( e->extentData - (char *)e );
                else
                    ofs = e->lastRecord.getOfs() + e->lastRecord.rec()->lengthWithHeaders;
                DiskLoc dl( ext.a(), ofs );
                Record *r = dl.rec();
                r->lengthWithHeaders = Record::HeaderSize + len;
                r->extentOfs = e->myLoc.getOfs();
                r->nextOfs = DiskLoc::NullOfs;
                r->prevOfs = e->lastRecord.isNull() ? DiskLoc::NullOfs : e->lastRecord.getOfs();
                memcpy( r->data, o.objdata(), len );
                if ( e->firstRecord.isNull() )
                    e->firstRecord = dl;
                else
                    e->lastRecord.rec()->nextOfs = ofs;
                e->lastRecord = dl;
                return dl;
            }
            static const char *ns() {
                return "unittests.ScanCapped";
            }
            static NamespaceDetails *nsd() {
                return nsdetails( ns() );
            }
        private:
            dblock lk_;
        };

        class Empty : public Base {
            virtual void prepare() {}
            virtual int count() const {
                return 0;
            }
        };

        class EmptyLooped : public Base {
            virtual void prepare() {
                nsd()->capFirstNewRecord = DiskLoc();
            }
            virtual int count() const {
                return 0;
            }
        };

        class EmptyMultiExtentLooped : public Base {
            virtual void prepare() {
                nsd()->capFirstNewRecord = DiskLoc();
            }
            virtual int count() const {
                return 0;
            }
            virtual int nExtents() const {
                return 3;
            }
        };

        class Single : public Base {
            virtual void prepare() {
                nsd()->capFirstNewRecord = insert( nsd()->capExtent, 0 );
            }
            virtual int count() const {
                return 1;
            }
        };

        class NewCapFirst : public Base {
            virtual void prepare() {
                nsd()->capFirstNewRecord = insert( nsd()->capExtent, 0 );
                insert( nsd()->capExtent, 1 );
            }
            virtual int count() const {
                return 2;
            }
        };

        class NewCapLast : public Base {
            virtual void prepare() {
                insert( nsd()->capExtent, 0 );
                nsd()->capFirstNewRecord = insert( nsd()->capExtent, 1 );
            }
            virtual int count() const {
                return 2;
            }
        };

        class NewCapMiddle : public Base {
            virtual void prepare() {
                insert( nsd()->capExtent, 0 );
                nsd()->capFirstNewRecord = insert( nsd()->capExtent, 1 );
                insert( nsd()->capExtent, 2 );
            }
            virtual int count() const {
                return 3;
            }
        };

        class FirstExtent : public Base {
            virtual void prepare() {
                insert( nsd()->capExtent, 0 );
                insert( nsd()->lastExtent, 1 );
                nsd()->capFirstNewRecord = insert( nsd()->capExtent, 2 );
                insert( nsd()->capExtent, 3 );
            }
            virtual int count() const {
                return 4;
            }
            virtual int nExtents() const {
                return 2;
            }
        };

        class LastExtent : public Base {
            virtual void prepare() {
                nsd()->capExtent = nsd()->lastExtent;
                insert( nsd()->capExtent, 0 );
                insert( nsd()->firstExtent, 1 );
                nsd()->capFirstNewRecord = insert( nsd()->capExtent, 2 );
                insert( nsd()->capExtent, 3 );
            }
            virtual int count() const {
                return 4;
            }
            virtual int nExtents() const {
                return 2;
            }
        };

        class MidExtent : public Base {
            virtual void prepare() {
                nsd()->capExtent = nsd()->firstExtent.ext()->xnext;
                insert( nsd()->capExtent, 0 );
                insert( nsd()->lastExtent, 1 );
                insert( nsd()->firstExtent, 2 );
                nsd()->capFirstNewRecord = insert( nsd()->capExtent, 3 );
                insert( nsd()->capExtent, 4 );
            }
            virtual int count() const {
                return 5;
            }
            virtual int nExtents() const {
                return 3;
            }
        };

        class AloneInExtent : public Base {
            virtual void prepare() {
                nsd()->capExtent = nsd()->firstExtent.ext()->xnext;
                insert( nsd()->lastExtent, 0 );
                insert( nsd()->firstExtent, 1 );
                nsd()->capFirstNewRecord = insert( nsd()->capExtent, 2 );
            }
            virtual int count() const {
                return 3;
            }
            virtual int nExtents() const {
                return 3;
            }
        };

        class FirstInExtent : public Base {
            virtual void prepare() {
                nsd()->capExtent = nsd()->firstExtent.ext()->xnext;
                insert( nsd()->lastExtent, 0 );
                insert( nsd()->firstExtent, 1 );
                nsd()->capFirstNewRecord = insert( nsd()->capExtent, 2 );
                insert( nsd()->capExtent, 3 );
            }
            virtual int count() const {
                return 4;
            }
            virtual int nExtents() const {
                return 3;
            }
        };

        class LastInExtent : public Base {
            virtual void prepare() {
                nsd()->capExtent = nsd()->firstExtent.ext()->xnext;
                insert( nsd()->capExtent, 0 );
                insert( nsd()->lastExtent, 1 );
                insert( nsd()->firstExtent, 2 );
                nsd()->capFirstNewRecord = insert( nsd()->capExtent, 3 );
            }
            virtual int count() const {
                return 4;
            }
            virtual int nExtents() const {
                return 3;
            }
        };

    } // namespace ScanCapped
    
    namespace Insert {
        class Base {
        public:
            Base() {
                setClient( ns() );
            }
            virtual ~Base() {
                if ( !nsd() )
                    return;
                string n( ns() );
                dropNS( n );
            }
        protected:
            static const char *ns() {
                return "unittests.pdfiletests.Insert";
            }
            static NamespaceDetails *nsd() {
                return nsdetails( ns() );
            }
        private:
            dblock lk_;
        };
        
        class UpdateDate : public Base {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendTimestamp( "a" );
                BSONObj o = b.done();
                ASSERT( 0 == o.getField( "a" ).date() );
                theDataFileMgr.insert( ns(), o );
                ASSERT( 0 != o.getField( "a" ).date() );
            }
        };
    } // namespace Insert
    
    class All : public Suite {
    public:
        All() : Suite( "pdfile" ){}
        
        void setupTests(){
            add< ScanCapped::Empty >();
            add< ScanCapped::EmptyLooped >();
            add< ScanCapped::EmptyMultiExtentLooped >();
            add< ScanCapped::Single >();
            add< ScanCapped::NewCapFirst >();
            add< ScanCapped::NewCapLast >();
            add< ScanCapped::NewCapMiddle >();
            add< ScanCapped::FirstExtent >();
            add< ScanCapped::LastExtent >();
            add< ScanCapped::MidExtent >();
            add< ScanCapped::AloneInExtent >();
            add< ScanCapped::FirstInExtent >();
            add< ScanCapped::LastInExtent >();
            add< Insert::UpdateDate >();
        }
    } myall;

} // namespace PdfileTests

