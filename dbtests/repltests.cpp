// repltests.cpp : Unit tests for replication
//

/**
 *    Copyright (C) 2009 10gen Inc.
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
#include "../db/repl.h"

#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"

#include "dbtests.h"

namespace mongo {
    void createOplog();
}

namespace ReplTests {

    BSONObj f( const char *s ) {
        return fromjson( s );
    }    
    
    class Base {
        dblock lk;
        Client::Context _context;
    public:
        Base() : _context( ns() ){
            replSettings.master = true;
            createOplog();
            ensureHaveIdIndex( ns() );
        }
        ~Base() {
            try {
                replSettings.master = false;
                deleteAll( ns() );
                deleteAll( cllNS() );
            } catch ( ... ) {
                FAIL( "Exception while cleaning up test" );
            }
        }
    protected:
        static const char *ns() {
            return "unittests.repltests";
        }
        static const char *cllNS() {
            return "local.oplog.$main";
        }
        DBDirectClient *client() const { return &client_; }
        BSONObj one( const BSONObj &query = BSONObj() ) const {
            return client()->findOne( ns(), query );            
        }
        void checkOne( const BSONObj &o ) const {
            check( o, one( o ) );
        }
        void checkAll( const BSONObj &o ) const {
            auto_ptr< DBClientCursor > c = client()->query( ns(), o );
            assert( c->more() );
            while( c->more() ) {
                check( o, c->next() );
            }
        }
        void check( const BSONObj &expected, const BSONObj &got ) const {
            if ( expected.woCompare( got ) ) {
                out() << "expected: " << expected.toString()
                    << ", got: " << got.toString() << endl;
            }
            ASSERT_EQUALS( expected , got );
        }
        BSONObj oneOp() const { 
            return client()->findOne( cllNS(), BSONObj() );
        }
        int count() const {
            int count = 0;
            dblock lk;
            Client::Context ctx( ns() );
            auto_ptr< Cursor > c = theDataFileMgr.findAll( ns() );
            for(; c->ok(); c->advance(), ++count ) {
//                cout << "obj: " << c->current().toString() << endl;
            }
            return count;
        }
        static int opCount() {
            dblock lk;
            Client::Context ctx( cllNS() );
            int count = 0;
            for( auto_ptr< Cursor > c = theDataFileMgr.findAll( cllNS() ); c->ok(); c->advance() )
                ++count;
            return count;
        }
        static void applyAllOperations() {
            class Applier : public ReplSource {
            public:
                static void apply( const BSONObj &op ) {
                    ReplSource::applyOperation( op );
                }
            };
            dblock lk;
            vector< BSONObj > ops;
            {
                Client::Context ctx( cllNS() );
                for( auto_ptr< Cursor > c = theDataFileMgr.findAll( cllNS() ); c->ok(); c->advance() )
                    ops.push_back( c->current() );
            }
            {
                Client::Context ctx( ns() );
                for( vector< BSONObj >::iterator i = ops.begin(); i != ops.end(); ++i )
                    Applier::apply( *i );
            }
        }
        static void printAll( const char *ns ) {
            dblock lk;
            Client::Context ctx( ns );
            auto_ptr< Cursor > c = theDataFileMgr.findAll( ns );
            vector< DiskLoc > toDelete;
            out() << "all for " << ns << endl;
            for(; c->ok(); c->advance() ) {
                out() << c->current().toString() << endl;
            }            
        }
        // These deletes don't get logged.
        static void deleteAll( const char *ns ) {
            dblock lk;
            Client::Context ctx( ns );
            auto_ptr< Cursor > c = theDataFileMgr.findAll( ns );
            vector< DiskLoc > toDelete;
            for(; c->ok(); c->advance() ) {
                toDelete.push_back( c->currLoc() );
            }
            for( vector< DiskLoc >::iterator i = toDelete.begin(); i != toDelete.end(); ++i ) {
                theDataFileMgr.deleteRecord( ns, i->rec(), *i, true );            
            }
        }
        static void insert( const BSONObj &o, bool god = false ) {
            dblock lk;
            Client::Context ctx( ns() );
            theDataFileMgr.insert( ns(), o.objdata(), o.objsize(), god );
        }
        static BSONObj wid( const char *json ) {
            class BSONObjBuilder b;
            OID id;
            id.init();
            b.appendOID( "_id", &id );
            b.appendElements( fromjson( json ) );
            return b.obj();
        }
    private:
        static DBDirectClient client_;
    };
    DBDirectClient Base::client_;
    
    class LogBasic : public Base {
    public:
        void run() {
            ASSERT_EQUALS( 1, opCount() );
            client()->insert( ns(), fromjson( "{\"a\":\"b\"}" ) );
            ASSERT_EQUALS( 2, opCount() );
        }
    };
    
    namespace Idempotence {
        
        class Base : public ReplTests::Base {
        public:
            virtual ~Base() {}
            void run() {
                reset();
                doIt();
                int nOps = opCount();
                check();
                applyAllOperations();
                check();
                ASSERT_EQUALS( nOps, opCount() );
                
                reset();
                applyAllOperations();
                check();
                ASSERT_EQUALS( nOps, opCount() );
                applyAllOperations();
                check();
                ASSERT_EQUALS( nOps, opCount() );
            }
        protected:
            virtual void doIt() const = 0;
            virtual void check() const = 0;
            virtual void reset() const = 0;
        };
        
        class InsertTimestamp : public Base {
        public:
            void doIt() const {
                BSONObjBuilder b;
                b.append( "a", 1 );
                b.appendTimestamp( "t" );
                client()->insert( ns(), b.done() );
                date_ = client()->findOne( ns(), QUERY( "a" << 1 ) ).getField( "t" ).date();
            }
            void check() const {
                BSONObj o = client()->findOne( ns(), QUERY( "a" << 1 ) );
                ASSERT( 0 != o.getField( "t" ).date() );
                ASSERT_EQUALS( date_, o.getField( "t" ).date() );
            }
            void reset() const {
                deleteAll( ns() );
            }
        private:
            mutable Date_t date_;
        };
        
        class InsertAutoId : public Base {
        public:
            InsertAutoId() : o_( fromjson( "{\"a\":\"b\"}" ) ) {}
            void doIt() const {
                client()->insert( ns(), o_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
            }
            void reset() const {
                deleteAll( ns() );
            }
        protected:
            BSONObj o_;
        };

        class InsertWithId : public InsertAutoId {
        public:
            InsertWithId() {
                o_ = fromjson( "{\"_id\":ObjectId(\"0f0f0f0f0f0f0f0f0f0f0f0f\"),\"a\":\"b\"}" );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( o_ );
            }
        };
        
        class InsertTwo : public Base {
        public:
            InsertTwo() : 
            o_( fromjson( "{'_id':1,a:'b'}" ) ),
            t_( fromjson( "{'_id':2,c:'d'}" ) ) {}
            void doIt() const {
                vector< BSONObj > v;
                v.push_back( o_ );
                v.push_back( t_ );
                client()->insert( ns(), v );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
                checkOne( o_ );
                checkOne( t_ );
            }
            void reset() const {
                deleteAll( ns() );
            }
        private:
            BSONObj o_;
            BSONObj t_;
        };

        class InsertTwoIdentical : public Base {
        public:
            InsertTwoIdentical() : o_( fromjson( "{\"a\":\"b\"}" ) ) {}
            void doIt() const {
                client()->insert( ns(), o_ );
                client()->insert( ns(), o_ );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
            }
            void reset() const {
                deleteAll( ns() );
            }
        private:
            BSONObj o_;            
        };

        class UpdateTimestamp : public Base {
        public:
            void doIt() const {
                BSONObjBuilder b;
                b.append( "_id", 1 );
                b.appendTimestamp( "t" );
                client()->update( ns(), BSON( "_id" << 1 ), b.done() );
                date_ = client()->findOne( ns(), QUERY( "_id" << 1 ) ).getField( "t" ).date();
            }
            void check() const {
                BSONObj o = client()->findOne( ns(), QUERY( "_id" << 1 ) );
                ASSERT( 0 != o.getField( "t" ).date() );
                ASSERT_EQUALS( date_, o.getField( "t" ).date() );
            }
            void reset() const {
                deleteAll( ns() );
                insert( BSON( "_id" << 1 ) );
            }
        private:
            mutable Date_t date_;
        };
        
        class UpdateSameField : public Base {
        public:
            UpdateSameField() :
            q_( fromjson( "{a:'b'}" ) ),
            o1_( wid( "{a:'b'}" ) ),
            o2_( wid( "{a:'b'}" ) ),
            u_( fromjson( "{a:'c'}" ) ){}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
                ASSERT( !client()->findOne( ns(), q_ ).isEmpty() );
                ASSERT( !client()->findOne( ns(), u_ ).isEmpty() );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o1_ );
                insert( o2_ );
            }
        private:
            BSONObj q_, o1_, o2_, u_;
        };        
        
        class UpdateSameFieldWithId : public Base {
        public:
            UpdateSameFieldWithId() :
            o_( fromjson( "{'_id':1,a:'b'}" ) ),
            q_( fromjson( "{a:'b'}" ) ),
            u_( fromjson( "{'_id':1,a:'c'}" ) ){}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
                ASSERT( !client()->findOne( ns(), q_ ).isEmpty() );
                ASSERT( !client()->findOne( ns(), u_ ).isEmpty() );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
                insert( fromjson( "{'_id':2,a:'b'}" ) );
            }
        private:
            BSONObj o_, q_, u_;            
        };        

        class UpdateSameFieldExplicitId : public Base {
        public:
            UpdateSameFieldExplicitId() :
            o_( fromjson( "{'_id':1,a:'b'}" ) ),
            u_( fromjson( "{'_id':1,a:'c'}" ) ){}
            void doIt() const {
                client()->update( ns(), o_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( u_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, u_;            
        };
        
        class UpdateId : public UpdateSameFieldExplicitId {
        public:
            UpdateId() {
                o_ = fromjson( "{'_id':1}" );
                u_ = fromjson( "{'_id':2}" );
            }
        };
        
        class UpdateDifferentFieldExplicitId : public Base {
        public:
            UpdateDifferentFieldExplicitId() :
            o_( fromjson( "{'_id':1,a:'b'}" ) ),
            q_( fromjson( "{'_id':1}" ) ),
            u_( fromjson( "{'_id':1,a:'c'}" ) ){}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( u_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_;            
        };        
        
        class UpsertUpdateNoMods : public UpdateDifferentFieldExplicitId {
            void doIt() const {
                client()->update( ns(), q_, u_, true );
            }
        };
        
        class UpsertInsertNoMods : public InsertAutoId {
            void doIt() const {
                client()->update( ns(), fromjson( "{a:'c'}" ), o_, true );
            }
        };
        
        class UpdateSet : public Base {
        public:
            UpdateSet() :
            o_( fromjson( "{'_id':1,a:5}" ) ),
            q_( fromjson( "{a:5}" ) ),
            u_( fromjson( "{$set:{a:7}}" ) ),
            ou_( fromjson( "{'_id':1,a:7}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_, ou_;            
        };
        
        class UpdateInc : public Base {
        public:
            UpdateInc() :
            o_( fromjson( "{'_id':1,a:5}" ) ),
            q_( fromjson( "{a:5}" ) ),
            u_( fromjson( "{$inc:{a:3}}" ) ),
            ou_( fromjson( "{'_id':1,a:8}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_, ou_;            
        };

        class UpdateInc2 : public Base {
        public:
            UpdateInc2() :
            o_( fromjson( "{'_id':1,a:5}" ) ),
            q_( fromjson( "{a:5}" ) ),
            u_( fromjson( "{$inc:{a:3},$set:{x:5}}" ) ),
            ou_( fromjson( "{'_id':1,a:8,x:5}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_, ou_;            
        };
        
        class IncEmbedded : public Base {
        public:
            IncEmbedded() :
            o_( fromjson( "{'_id':1,a:{b:3},b:{b:1}}" ) ),
            q_( fromjson( "{'_id':1}" ) ),
            u_( fromjson( "{$inc:{'a.b':1,'b.b':1}}" ) ),
            ou_( fromjson( "{'_id':1,a:{b:4},b:{b:2}}" ) )
            {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_, ou_;            
        };

        class IncCreates : public Base {
        public:
            IncCreates() :
            o_( fromjson( "{'_id':1}" ) ),
            q_( fromjson( "{'_id':1}" ) ),
            u_( fromjson( "{$inc:{'a':1}}" ) ),
            ou_( fromjson( "{'_id':1,a:1}") )
            {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_, ou_;            
        };


        class UpsertInsertIdMod : public Base {
        public:
            UpsertInsertIdMod() :
            q_( fromjson( "{'_id':5,a:4}" ) ),
            u_( fromjson( "{$inc:{a:3}}" ) ),
            ou_( fromjson( "{'_id':5,a:7}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_, true );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
            }
        protected:
            BSONObj q_, u_, ou_;            
        };
        
        class UpsertInsertSet : public Base {
        public:
            UpsertInsertSet() :
            q_( fromjson( "{a:5}" ) ),
            u_( fromjson( "{$set:{a:7}}" ) ),
            ou_( fromjson( "{a:7}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_, true );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
                ASSERT( !client()->findOne( ns(), ou_ ).isEmpty() );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':7,a:7}" ) );
            }
        protected:
            BSONObj o_, q_, u_, ou_;            
        };
        
        class UpsertInsertInc : public Base {
        public:
            UpsertInsertInc() :
            q_( fromjson( "{a:5}" ) ),
            u_( fromjson( "{$inc:{a:3}}" ) ),
            ou_( fromjson( "{a:8}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_, true );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                ASSERT( !client()->findOne( ns(), ou_ ).isEmpty() );
            }
            void reset() const {
                deleteAll( ns() );
            }
        protected:
            BSONObj o_, q_, u_, ou_;            
        };
        
        class MultiInc : public Base {
        public:
            
            string s() const {
                stringstream ss;
                auto_ptr<DBClientCursor> cc = client()->query( ns() , Query().sort( BSON( "_id" << 1 ) ) );
                bool first = true;
                while ( cc->more() ){
                    if ( first ) first = false;
                    else ss << ",";
                    
                    BSONObj o = cc->next();
                    ss << o["x"].numberInt();
                }
                return ss.str();
            }
            
            void doIt() const {
                client()->insert( ns(), BSON( "_id" << 1 << "x" << 1 ) );
                client()->insert( ns(), BSON( "_id" << 2 << "x" << 5 ) );
                
                ASSERT_EQUALS( "1,5" , s() );
                
                client()->update( ns() , BSON( "_id" << 1 ) , BSON( "$inc" << BSON( "x" << 1 ) ) );
                ASSERT_EQUALS( "2,5" , s() );
                
                client()->update( ns() , BSONObj() , BSON( "$inc" << BSON( "x" << 1 ) ) );
                ASSERT_EQUALS( "3,5" , s() );
                
                client()->update( ns() , BSONObj() , BSON( "$inc" << BSON( "x" << 1 ) ) , false , true );
                check();
            }

            void check() const {
                ASSERT_EQUALS( "4,6" , s() );
            }
            
            void reset() const {
                deleteAll( ns() );
            }
        };
        
        class UpdateWithoutPreexistingId : public Base {
        public:
            UpdateWithoutPreexistingId() :
            o_( fromjson( "{a:5}" ) ),
            u_( fromjson( "{a:5}" ) ),
            ot_( fromjson( "{b:4}" ) ) {}
            void doIt() const {
                client()->update( ns(), o_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
                checkOne( u_ );
                checkOne( ot_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( ot_, true );
                insert( o_, true );
            }
        protected:
            BSONObj o_, u_, ot_;            
        };        
        
        class Remove : public Base {
        public:
            Remove() :
            o1_( f( "{\"_id\":\"010101010101010101010101\",\"a\":\"b\"}" ) ),
            o2_( f( "{\"_id\":\"010101010101010101010102\",\"a\":\"b\"}" ) ),
            q_( f( "{\"a\":\"b\"}" ) ) {}
            void doIt() const {
                client()->remove( ns(), q_ );
            }
            void check() const {
                ASSERT_EQUALS( 0, count() );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o1_ );
                insert( o2_ );
            }
        protected:
            BSONObj o1_, o2_, q_;            
        };
        
        class RemoveOne : public Remove {
            void doIt() const {
                client()->remove( ns(), q_, true );
            }            
            void check() const {
                ASSERT_EQUALS( 1, count() );
            }
        };
          
        class FailingUpdate : public Base {
        public:
            FailingUpdate() :
            o_( fromjson( "{'_id':1,a:'b'}" ) ),
            u_( fromjson( "{'_id':1,c:'d'}" ) ) {}
            void doIt() const {
                client()->update( ns(), o_, u_ );
                client()->insert( ns(), o_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( o_ );
            }
            void reset() const {
                deleteAll( ns() );
            }
        protected:
            BSONObj o_, u_;
        };
        
        class SetNumToStr : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$set" << BSON( "a" << "bcd" ) ) );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( BSON( "_id" << 0 << "a" << "bcd" ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( BSON( "_id" << 0 << "a" << 4.0 ) );
            }
        };
        
        class Push : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$push" << BSON( "a" << 5.0 ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4]}" ) );
            }            
        };
        
        class PushUpsert : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$push" << BSON( "a" << 5.0 ) ), true );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4]}" ) );
            }            
        };

        class MultiPush : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$push" << BSON( "a" << 5.0 ) << "$push" << BSON( "b.c" << 6.0 ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5],b:{c:[6]}}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4]}" ) );
            }            
        };

        class EmptyPush : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$push" << BSON( "a" << 5.0 ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0}" ) );
            }                        
        };

        class PushAll : public Base {
        public:
            void doIt() const {
	      client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pushAll:{a:[5.0,6.0]}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5,6]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4]}" ) );
            }            
        };
        
        class PushAllUpsert : public Base {
        public:
            void doIt() const {
	      client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pushAll:{a:[5.0,6.0]}}" ), true );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5,6]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4]}" ) );
            }            
        };

        class EmptyPushAll : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pushAll:{a:[5.0,6.0]}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[5,6]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0}" ) );
            }                        
        };

        class Pull : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$pull" << BSON( "a" << 4.0 ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4,5]}" ) );
            }            
        };
                
        class PullNothing : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$pull" << BSON( "a" << 6.0 ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4,5]}" ) );
            }            
        };
                
        class PullAll : public Base {
        public:
            void doIt() const {
	      client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pullAll:{a:[4,5]}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[6]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4,5,6]}" ) );
            }            
        };

        class Pop : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pop:{a:1}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4,5,6]}" ) );
            }            
        };

        class PopReverse : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pop:{a:-1}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[5,6]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4,5,6]}" ) );
            }            
        };

        class BitOp : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$bit:{a:{and:2,or:8}}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( BSON( "_id" << 0 << "a" << ( ( 3 & 2 ) | 8 ) ) , one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:3}" ) );
            }            
        };

        

    } // namespace Idempotence
    
    class DeleteOpIsIdBased : public Base {
    public:
        void run() {
            insert( BSON( "_id" << 0 << "a" << 10 ) );
            insert( BSON( "_id" << 1 << "a" << 11 ) );
            insert( BSON( "_id" << 3 << "a" << 10 ) );
            client()->remove( ns(), BSON( "a" << 10 ) );
            ASSERT_EQUALS( 1U, client()->count( ns(), BSONObj() ) );
            insert( BSON( "_id" << 0 << "a" << 11 ) );
            insert( BSON( "_id" << 2 << "a" << 10 ) );            
            insert( BSON( "_id" << 3 << "a" << 10 ) );
            
            applyAllOperations();
            ASSERT_EQUALS( 2U, client()->count( ns(), BSONObj() ) );
            ASSERT( !one( BSON( "_id" << 1 ) ).isEmpty() );
            ASSERT( !one( BSON( "_id" << 2 ) ).isEmpty() );
        }
    };
    
    class DbIdsTest {
    public:
        void run() {
            Client::Context ctx( "unittests.repltest.DbIdsTest" );
            
            s_.reset( new DbIds( "local.temp.DbIdsTest" ) );
            s_->reset();
            check( false, false, false );

            s_->set( "a", BSON( "_id" << 4 ), true );
            check( true, false, false );
            s_->set( "a", BSON( "_id" << 4 ), false );
            check( false, false, false );
            
            s_->set( "b", BSON( "_id" << 4 ), true );
            check( false, true, false );
            s_->set( "b", BSON( "_id" << 4 ), false );
            check( false, false, false );

            s_->set( "a", BSON( "_id" << 5 ), true );
            check( false, false, true );
            s_->set( "a", BSON( "_id" << 5 ), false );
            check( false, false, false );

            s_->set( "a", BSON( "_id" << 4 ), true );
            s_->set( "b", BSON( "_id" << 4 ), true );
            s_->set( "a", BSON( "_id" << 5 ), true );
            check( true, true, true );

            s_->reset();
            check( false, false, false );
            
            s_->set( "a", BSON( "_id" << 4 ), true );
            s_->set( "a", BSON( "_id" << 4 ), true );
            check( true, false, false );
            s_->set( "a", BSON( "_id" << 4 ), false );
            check( false, false, false );
        }
    private:
        void check( bool one, bool two, bool three ) {
            ASSERT_EQUALS( one, s_->get( "a", BSON( "_id" << 4 ) ) );
            ASSERT_EQUALS( two, s_->get( "b", BSON( "_id" << 4 ) ) );
            ASSERT_EQUALS( three, s_->get( "a", BSON( "_id" << 5 ) ) );            
        }
        dblock lk_;
        auto_ptr< DbIds > s_;
    };
    
    class MemIdsTest {
    public:
        void run() {
            int n = sizeof( BSONObj ) + BSON( "_id" << 4 ).objsize();
            
            s_.reset();
            ASSERT_EQUALS( 0, s_.roughSize() );
            ASSERT( !s_.get( "a", BSON( "_id" << 4 ) ) );
            ASSERT( !s_.get( "b", BSON( "_id" << 4 ) ) );
            s_.set( "a", BSON( "_id" << 4 ), true );
            ASSERT_EQUALS( n, s_.roughSize() );
            ASSERT( s_.get( "a", BSON( "_id" << 4 ) ) );
            ASSERT( !s_.get( "b", BSON( "_id" << 4 ) ) );
            s_.set( "a", BSON( "_id" << 4 ), false );
            ASSERT_EQUALS( 0, s_.roughSize() );
            ASSERT( !s_.get( "a", BSON( "_id" << 4 ) ) );

            s_.set( "a", BSON( "_id" << 4 ), true );
            s_.set( "b", BSON( "_id" << 4 ), true );
            s_.set( "b", BSON( "_id" << 100 ), true );
            s_.set( "b", BSON( "_id" << 101 ), true );
            ASSERT_EQUALS( n * 4, s_.roughSize() );
        }
    private:
        MemIds s_;
    };

    class IdTrackerTest {
    public:
        void run() {
            Client::Context ctx( "unittests.repltests.IdTrackerTest" );
            
            ASSERT( s_.inMem() );
            s_.reset( 4 * sizeof( BSONObj ) - 1 );
            s_.haveId( "a", BSON( "_id" << 0 ), true );
            s_.haveId( "a", BSON( "_id" << 1 ), true );
            s_.haveId( "b", BSON( "_id" << 0 ), true );
            s_.haveModId( "b", BSON( "_id" << 0 ), true );
            ASSERT( s_.inMem() );
            check();
            s_.mayUpgradeStorage();
            ASSERT( !s_.inMem() );
            check();
            
            s_.haveId( "a", BSON( "_id" << 1 ), false );
            ASSERT( !s_.haveId( "a", BSON( "_id" << 1 ) ) );
            s_.haveId( "a", BSON( "_id" << 1 ), true );
            check();
            ASSERT( !s_.inMem() );            
            
            s_.reset();
            ASSERT( s_.inMem() );                        
        }
    private:
        void check() {
            ASSERT( s_.haveId( "a", BSON( "_id" << 0 ) ) );
            ASSERT( s_.haveId( "a", BSON( "_id" << 1 ) ) );
            ASSERT( s_.haveId( "b", BSON( "_id" << 0 ) ) );
            ASSERT( s_.haveModId( "b", BSON( "_id" << 0 ) ) );            
        }
        dblock lk_;
        IdTracker s_;
    };
    
    class All : public Suite {
    public:
        All() : Suite( "repl" ){
        }
        
        void setupTests(){
            add< LogBasic >();
            add< Idempotence::InsertTimestamp >();
            add< Idempotence::InsertAutoId >();
            add< Idempotence::InsertWithId >();
            add< Idempotence::InsertTwo >();
            add< Idempotence::InsertTwoIdentical >();
            add< Idempotence::UpdateTimestamp >();
            add< Idempotence::UpdateSameField >();
            add< Idempotence::UpdateSameFieldWithId >();
            add< Idempotence::UpdateSameFieldExplicitId >();
            add< Idempotence::UpdateId >();
            add< Idempotence::UpdateDifferentFieldExplicitId >();
            add< Idempotence::UpsertUpdateNoMods >();
            add< Idempotence::UpsertInsertNoMods >();
            add< Idempotence::UpdateSet >();
            add< Idempotence::UpdateInc >();
            add< Idempotence::UpdateInc2 >();
            add< Idempotence::IncEmbedded >(); // SERVER-716
            add< Idempotence::IncCreates >(); // SERVER-717
            add< Idempotence::UpsertInsertIdMod >();
            add< Idempotence::UpsertInsertSet >();
            add< Idempotence::UpsertInsertInc >();
            add< Idempotence::MultiInc >();
            // Don't worry about this until someone wants this functionality.
//            add< Idempotence::UpdateWithoutPreexistingId >();
            add< Idempotence::Remove >();
            add< Idempotence::RemoveOne >();
            add< Idempotence::FailingUpdate >();
            add< Idempotence::SetNumToStr >();
            add< Idempotence::Push >();
            add< Idempotence::PushUpsert >();
            add< Idempotence::MultiPush >();
            add< Idempotence::EmptyPush >();
            add< Idempotence::PushAll >();
            add< Idempotence::PushAllUpsert >();
            add< Idempotence::EmptyPushAll >();
            add< Idempotence::Pull >();
            add< Idempotence::PullNothing >();
            add< Idempotence::PullAll >();
            add< Idempotence::Pop >();
            add< Idempotence::PopReverse >();
            add< Idempotence::BitOp >();
            add< DeleteOpIsIdBased >();
            add< DbIdsTest >();
            add< MemIdsTest >();
            add< IdTrackerTest >();
        }
    } myall;
    
} // namespace ReplTests

