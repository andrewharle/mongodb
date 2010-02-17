// features1.js

s = new ShardingTest( "features1" , 2 , 1 , 1 );

s.adminCommand( { enablesharding : "test" } );

// ---- can't shard system namespaces ----

assert( ! s.admin.runCommand( { shardcollection : "test.system.blah" , key : { num : 1 } } ).ok , "shard system namespace" );

// ---- setup test.foo -----

s.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );

db = s.getDB( "test" );

a = s._connections[0].getDB( "test" );
b = s._connections[1].getDB( "test" );

db.foo.ensureIndex( { y : 1 } );

s.adminCommand( { split : "test.foo" , middle : { num : 10 } } );
s.adminCommand( { movechunk : "test.foo" , find : { num : 20 } , to : s.getOther( s.getServer( "test" ) ).name } );

db.foo.save( { num : 5 } );
db.foo.save( { num : 15 } );

s.sync();

// ---- make sure shard key index is everywhere ----

assert.eq( 3 , a.foo.getIndexKeys().length , "a index 1" );
assert.eq( 3 , b.foo.getIndexKeys().length , "b index 1" );

// ---- make sure if you add an index it goes everywhere ------

db.foo.ensureIndex( { x : 1 } );

s.sync();

assert.eq( 4 , a.foo.getIndexKeys().length , "a index 2" );
assert.eq( 4 , b.foo.getIndexKeys().length , "b index 2" );

// ---- no unique indexes ------

db.foo.ensureIndex( { z : 1 } , true );

s.sync();

assert.eq( 4 , a.foo.getIndexKeys().length , "a index 3" );
assert.eq( 4 , b.foo.getIndexKeys().length , "b index 3" );

// ---- can't shard thing with unique indexes

db.foo2.ensureIndex( { a : 1 } );
s.sync();
assert( s.admin.runCommand( { shardcollection : "test.foo2" , key : { num : 1 } } ).ok , "shard with index" );

db.foo3.ensureIndex( { a : 1 } , true );
s.sync();
printjson( db.system.indexes.find( { ns : "test.foo3" } ).toArray() );
assert( ! s.admin.runCommand( { shardcollection : "test.foo3" , key : { num : 1 } } ).ok , "shard with unique index" );

// ----- eval -----

db.foo2.save( { num : 5 , a : 7 } );
db.foo3.save( { num : 5 , a : 8 } );

assert.eq( 1 , db.foo3.count() , "eval pre1" );
assert.eq( 1 , db.foo2.count() , "eval pre2" );

assert.eq( 8 , db.eval( function(){ return db.foo3.findOne().a; } ), "eval 1 " );
assert.throws( function(){ db.eval( function(){ return db.foo2.findOne().a; } ) } , "eval 2" )

assert.eq( 1 , db.eval( function(){ return db.foo3.count(); } ), "eval 3 " );
assert.throws( function(){ db.eval( function(){ return db.foo2.count(); } ) } , "eval 4" )


// ---- unique shard key ----

assert( s.admin.runCommand( { shardcollection : "test.foo4" , key : { num : 1 } , unique : true } ).ok , "shard with index and unique" );
s.adminCommand( { split : "test.foo4" , middle : { num : 10 } } );
s.adminCommand( { movechunk : "test.foo4" , find : { num : 20 } , to : s.getOther( s.getServer( "test" ) ).name } );
db.foo4.save( { num : 5 } );
db.foo4.save( { num : 15 } );
s.sync();
assert.eq( 1 , a.foo4.count() , "ua1" );
assert.eq( 1 , b.foo4.count() , "ub1" );

assert.eq( 2 , a.foo4.getIndexes().length , "ua2" );
assert.eq( 2 , b.foo4.getIndexes().length , "ub2" );

assert( a.foo4.getIndexes()[1].unique , "ua3" );
assert( b.foo4.getIndexes()[1].unique , "ub3" );

// --- don't let you convertToCapped ----
assert( ! db.foo4.isCapped() , "ca1" );
assert( ! a.foo4.isCapped() , "ca2" );
assert( ! b.foo4.isCapped() , "ca3" );
assert( ! db.foo4.convertToCapped( 30000 ).ok , "ca30" );
assert( ! db.foo4.isCapped() , "ca4" );
assert( ! a.foo4.isCapped() , "ca5" );
assert( ! b.foo4.isCapped() , "ca6" );

//      make sure i didn't break anything
db.foo4a.save( { a : 1 } );
assert( ! db.foo4a.isCapped() , "ca7" );
db.foo4a.convertToCapped( 30000 );
assert( db.foo4a.isCapped() , "ca8" );

// --- don't let you shard a capped collection

db.createCollection("foo5", {capped:true, size:30000});
assert( db.foo5.isCapped() , "cb1" );
assert( ! s.admin.runCommand( { shardcollection : "test.foo5" , key : { num : 1 } } ).ok , "shard capped" );


// ----- group ----

db.foo6.save( { a : 1 } );
db.foo6.save( { a : 3 } );
db.foo6.save( { a : 3 } );
s.sync();

assert.eq( 2 , db.foo6.group( { key : { a : 1 } , initial : { count : 0 } , 
                                reduce : function(z,prev){ prev.count++; } } ).length );

assert.eq( 3 , db.foo6.find().count() );
assert( s.admin.runCommand( { shardcollection : "test.foo6" , key : { a : 2 } } ).ok );
assert.eq( 3 , db.foo6.find().count() );

s.adminCommand( { split : "test.foo6" , middle : { a : 2 } } );
s.adminCommand( { movechunk : "test.foo6" , find : { a : 3 } , to : s.getOther( s.getServer( "test" ) ).name } );

assert.throws( function(){ db.foo6.group( { key : { a : 1 } , initial : { count : 0 } , reduce : function(z,prev){ prev.count++; } } ); } );;


s.stop()

