// Test cloneCollection command
// TODO(spencer): move this test out of slowNightly directory once there is a better place for tests
// that start their own mongod's but aren't slow
var baseName = "jstests_clonecollection";


ports = allocatePorts( 2 );

f = startMongod( "--port", ports[ 0 ], "--dbpath", MongoRunner.dataPath + baseName + "_from", "--nohttpinterface", "--bind_ip", "127.0.0.1" ).getDB( baseName );
t = startMongod( "--port", ports[ 1 ], "--dbpath", MongoRunner.dataPath + baseName + "_to", "--nohttpinterface", "--bind_ip", "127.0.0.1" ).getDB( baseName );

for( i = 0; i < 1000; ++i ) {
    f.a.save( { i: i } );
}
assert.eq( 1000, f.a.find().count() , "A1" );

assert.commandWorked( t.cloneCollection( "localhost:" + ports[ 0 ], "a" ) );
assert.eq( 1000, t.a.find().count() , "A2" );

t.a.drop();

assert.commandWorked( t.cloneCollection( "localhost:" + ports[ 0 ], "a", { i: { $gte: 10, $lt: 20 } } ) );
assert.eq( 10, t.a.find().count() , "A3" );

t.a.drop();
assert.eq( 0, t.system.indexes.find().count() , "prep 2");

f.a.ensureIndex( { i: 1 } );
assert.eq( 2, f.system.indexes.find().count(), "expected index missing" );
assert.commandWorked( t.cloneCollection( "localhost:" + ports[ 0 ], "a" ) );
if ( t.system.indexes.find().count() != 2 ) {
    printjson( t.system.indexes.find().toArray() );
}
assert.eq( 2, t.system.indexes.find().count(), "expected index missing" );
// Verify index works
x = t.a.find( { i: 50 } ).hint( { i: 1 } ).explain()
printjson( x )
assert.eq( 50, x.indexBounds.i[0][0] , "verify 1" );
assert.eq( 1, t.a.find( { i: 50 } ).hint( { i: 1 } ).toArray().length, "match length did not match expected" );

// Check that capped-ness is preserved on clone
f.a.drop();
t.a.drop();

f.createCollection( "a", {capped:true,size:1000} );
assert( f.a.isCapped() );
assert.commandWorked( t.cloneCollection( "localhost:" + ports[ 0 ], "a" ) );
assert( t.a.isCapped(), "cloned collection not capped" );

// Check that cloning to "system.profile" is disallowed.
f.a.drop();
f.system.profile.drop();
assert.commandWorked( f.setProfilingLevel( 2 ) );
f.a.insert( {} );
assert( !f.getLastError() );
assert.gt( f.system.profile.count(), 0 );
t.system.profile.drop();
assert.commandFailed( t.cloneCollection( "localhost:" + ports[ 0 ], "system.profile" ) );
