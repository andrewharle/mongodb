t = db.f
t.drop();
t.save( {} );
t.save( {} );
t.save( {} );

db.getMongo().getDB( "admin" ).runCommand( {closeAllDatabases:1} );

function test( want , msg ){
    var res = db.runCommand( { cursorInfo:1 } );
    assert.eq( want , res.clientCursors_size , msg + " " + tojson( res ) );
}

test( 0 , "A1" );
assert.eq( 3 , t.find().count() , "A1" );
assert.eq( 3 , t.find( {} ).count() , "A2" );
assert.eq( 2, t.find( {} ).limit( 2 ).itcount() , "A3" );
test( 1 , "B1" );

