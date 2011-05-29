// test read/write permissions

port = allocatePorts( 1 )[ 0 ];
baseName = "jstests_auth_auth1";

m = startMongod( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( "test" );

t = db[ baseName ];
t.drop();

users = db.getCollection( "system.users" );
users.remove( {} );

db.addUser( "eliot" , "eliot" );
db.addUser( "guest" , "guest", true );
db.getSisterDB( "admin" ).addUser( "super", "super" );

assert.throws( function() { t.findOne() }, [], "read without login" );

assert( db.auth( "eliot" , "eliot" ) , "auth failed" );

for( i = 0; i < 999; ++i ) {
    t.save( {i:i} );
}
assert.eq( 999, t.count() , "A1" );
assert.eq( 999, t.find().toArray().length , "A2" );

assert.eq( 999, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) , "A3" );
db.eval( function() { db[ "jstests_auth_auth1" ].save( {i:999} ) } );
assert.eq( 1000, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) , "A4" );

var p = { key : { i : true } , 
    reduce : function(obj,prev) { prev.count++; },
initial: { count: 0 }
};

assert.eq( 1000, t.group( p ).length , "A5" );

if ( db.runCommand( "features" ).readlock ){
    print( "doing readonly test" );
    assert( db.auth( "guest", "guest" ), "auth failed 2" );
    
    assert.eq( 1000, t.count() , "B1" );
    assert.eq( 1000, t.find().toArray().length , "B2" ); // make sure we have a getMore in play
    assert.commandWorked( db.runCommand( {ismaster:1} ) , "B3" );
    
    assert( !db.getLastError() , "B4" );
    t.save( {} ); // fail
    assert( db.getLastError() , "B5: " + tojson( db.getLastErrorObj() ) );
    assert.eq( 1000, t.count() , "B6" );
    
    assert.eq( 2, db.system.users.count() , "B7" );
    assert( !db.getLastError() , "B8" );
    db.addUser( "a", "b" );
    assert( db.getLastError() , "B9" );
    assert.eq( 2, db.system.users.count() , "B10");
    
    assert.eq( 1000, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) , "C1" );
    assert.eq( 1000, db.eval( function() { return db[ "jstests_auth_auth1" ].find().toArray().length; } ) , "C2" );
    db.eval( function() { db[ "jstests_auth_auth1" ].save( {i:1} ) } , "C3" );
    assert.eq( 1000, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) , "C4" );
    
    assert.eq( 1000, t.group( p ).length , "C5" );
    
    var p = { key : { i : true } , 
        reduce : function(obj,prev) { db.jstests_auth_auth1.save( {i:10000} ); prev.count++; },
              initial: { count: 0 }
            };
    
    assert.throws( function() { return t.group( p ) }, null , "write reduce didn't fail" );
}

