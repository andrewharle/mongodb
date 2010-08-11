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

// client.cpp

#include "pch.h"
#include "../client/dbclient.h"
#include "dbtests.h"
#include "../db/concurrency.h"
 
namespace ClientTests {
    
    class Base {
    public:
        
        Base( string coll ){
            _ns = (string)"test." + coll;
        }
        
        virtual ~Base(){
            db.dropCollection( _ns );
        }
        
        const char * ns(){ return _ns.c_str(); }
        
        string _ns;
        DBDirectClient db;
    };
        

    class DropIndex : public Base {
    public:
        DropIndex() : Base( "dropindex" ){}
        void run(){
            db.insert( ns() , BSON( "x" << 2 ) );
            ASSERT_EQUALS( 1 , db.getIndexes( ns() )->itcount() );
            
            db.ensureIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );
            
            db.dropIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 1 , db.getIndexes( ns() )->itcount() );
            
            db.ensureIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );

            db.dropIndexes( ns() );
            ASSERT_EQUALS( 1 , db.getIndexes( ns() )->itcount() );
        }
    };
    
    class ReIndex : public Base {
    public:
        ReIndex() : Base( "reindex" ){}
        void run(){
            
            db.insert( ns() , BSON( "x" << 2 ) );
            ASSERT_EQUALS( 1 , db.getIndexes( ns() )->itcount() );
            
            db.ensureIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );
            
            db.reIndex( ns() );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );
        }

    };

    class ReIndex2 : public Base {
    public:
        ReIndex2() : Base( "reindex2" ){}
        void run(){
            
            db.insert( ns() , BSON( "x" << 2 ) );
            ASSERT_EQUALS( 1 , db.getIndexes( ns() )->itcount() );
            
            db.ensureIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );
            
            BSONObj out;
            ASSERT( db.runCommand( "test" , BSON( "reIndex" << "reindex2" ) , out ) );
            ASSERT_EQUALS( 2 , out["nIndexes"].number() );
            ASSERT_EQUALS( 2 , db.getIndexes( ns() )->itcount() );
        }

    };

    class CS_10 : public Base {
    public:
        CS_10() : Base( "CS_10" ) {}
        void run() {
            string longs( 770, 'c' );
            for( int i = 0; i < 1111; ++i )
                db.insert( ns(), BSON( "a" << i << "b" << longs ) );
            db.ensureIndex( ns(), BSON( "a" << 1 << "b" << 1 ) );
            
            auto_ptr< DBClientCursor > c = db.query( ns(), Query().sort( BSON( "a" << 1 << "b" << 1 ) ) );
            ASSERT_EQUALS( 1111, c->itcount() );
        }
    };

    class PushBack : public Base {
    public:
        PushBack() : Base( "PushBack" ) {}
        void run() {
            for( int i = 0; i < 10; ++i )
                db.insert( ns(), BSON( "i" << i ) );
            auto_ptr< DBClientCursor > c = db.query( ns(), Query().sort( BSON( "i" << 1 ) ) );

            BSONObj o = c->next();
            ASSERT( c->more() );
            ASSERT_EQUALS( 9 , c->objsLeftInBatch() );
            ASSERT( c->moreInCurrentBatch() );

            c->putBack( o );
            ASSERT( c->more() );
            ASSERT_EQUALS( 10, c->objsLeftInBatch() );
            ASSERT( c->moreInCurrentBatch() );

            o = c->next();
            BSONObj o2 = c->next();
            BSONObj o3 = c->next();
            c->putBack( o3 );
            c->putBack( o2 );
            c->putBack( o );
            for( int i = 0; i < 10; ++i ) {
                o = c->next();
                ASSERT_EQUALS( i, o[ "i" ].number() );
            }
            ASSERT( !c->more() );
            ASSERT_EQUALS( 0, c->objsLeftInBatch() );
            ASSERT( !c->moreInCurrentBatch() );

            c->putBack( o );
            ASSERT( c->more() );
            ASSERT_EQUALS( 1, c->objsLeftInBatch() );
            ASSERT( c->moreInCurrentBatch() );
            ASSERT_EQUALS( 1, c->itcount() );
        }
    };

    class Create : public Base {
    public:
        Create() : Base( "Create" ) {}
        void run() {
            db.createCollection( "unittests.clienttests.create", 0, true );
            BSONObj info;
            ASSERT( db.runCommand( "unittests", BSON( "collstats" << "clienttests.create" ), info ) );
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "client" ){
        }

        void setupTests(){
            add<DropIndex>();
            add<ReIndex>();
            add<ReIndex2>();
            add<CS_10>();
            add<PushBack>();
            add<Create>();
        }
        
    } all;
}
