
test = new SyncCCTest( "sync1" )

db = test.conn.getDB( "test" )
t = db.sync1
t.save( { x : 1 } )
assert.eq( 1 , t.find().itcount() , "A1" );
assert.eq( 1 , t.find().count() , "A2" );
t.save( { x : 2 } )
assert.eq( 2 , t.find().itcount() , "A3" );
assert.eq( 2 , t.find().count() , "A4" );

test.checkHashes( "test" , "A3" );

test.tempKill();
assert.throws( function(){ t.save( { x : 3 } ) } , "B1" )
assert.eq( 2 , t.find().itcount() , "B2" );
test.tempStart();
test.checkHashes( "test" , "B3" );

test.stop();
