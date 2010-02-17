// shard2.js

/**
* test basic sharding
*/

placeCheck = function( num ){
    print("shard2 step: " + num );
}

s = new ShardingTest( "shard2" , 2 , 5 );

db = s.getDB( "test" );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );
assert.eq( 1 , s.config.chunks.count()  , "sanity check 1" );

s.adminCommand( { split : "test.foo" , middle : { num : 0 } } );
assert.eq( 2 , s.config.chunks.count() , "should be 2 shards" );
chunks = s.config.chunks.find().toArray();
assert.eq( chunks[0].shard , chunks[1].shard , "server should be the same after a split" );


db.foo.save( { num : 1 , name : "eliot" } );
db.foo.save( { num : 2 , name : "sara" } );
db.foo.save( { num : -1 , name : "joe" } );

s.adminCommand( "connpoolsync" );

assert.eq( 3 , s.getServer( "test" ).getDB( "test" ).foo.find().length() , "not right directly to db A" );
assert.eq( 3 , db.foo.find().length() , "not right on shard" );

primary = s.getServer( "test" ).getDB( "test" );
secondary = s.getOther( primary ).getDB( "test" );

assert.eq( 3 , primary.foo.find().length() , "primary wrong B" );
assert.eq( 0 , secondary.foo.find().length() , "secondary wrong C" );
assert.eq( 3 , db.foo.find().sort( { num : 1 } ).length() );

placeCheck( 2 );

// NOTE: at this point we have 2 shard on 1 server

// test move shard
assert.throws( function(){ s.adminCommand( { movechunk : "test.foo" , find : { num : 1 } , to : primary.getMongo().name } ); } );
assert.throws( function(){ s.adminCommand( { movechunk : "test.foo" , find : { num : 1 } , to : "adasd" } ) } );

s.adminCommand( { movechunk : "test.foo" , find : { num : 1 } , to : secondary.getMongo().name } );
assert.eq( 2 , secondary.foo.find().length() , "secondary should have 2 after move shard" );
assert.eq( 1 , primary.foo.find().length() , "primary should only have 1 after move shard" );

assert.eq( 2 , s.config.chunks.count() , "still should have 2 shards after move not:" + s.getChunksString() );
chunks = s.config.chunks.find().toArray();
assert.neq( chunks[0].shard , chunks[1].shard , "servers should NOT be the same after the move" );

placeCheck( 3 );

// test inserts go to right server/shard

db.foo.save( { num : 3 , name : "bob" } );
s.adminCommand( "connpoolsync" );
assert.eq( 1 , primary.foo.find().length() , "after move insert go wrong place?" );
assert.eq( 3 , secondary.foo.find().length() , "after move insert go wrong place?" );

db.foo.save( { num : -2 , name : "funny man" } );
s.adminCommand( "connpoolsync" );
assert.eq( 2 , primary.foo.find().length() , "after move insert go wrong place?" );
assert.eq( 3 , secondary.foo.find().length() , "after move insert go wrong place?" );


db.foo.save( { num : 0 , name : "funny guy" } );
s.adminCommand( "connpoolsync" );
assert.eq( 2 , primary.foo.find().length() , "boundary A" );
assert.eq( 4 , secondary.foo.find().length() , "boundary B" );

placeCheck( 4 );

// findOne
assert.eq( "eliot" , db.foo.findOne( { num : 1 } ).name );
assert.eq( "funny man" , db.foo.findOne( { num : -2 } ).name );

// getAll
function sumQuery( c ){
    var sum = 0;
    c.toArray().forEach(
        function(z){
            sum += z.num;
        }
    );
    return sum;
}
assert.eq( 6 , db.foo.find().length() , "sharded query 1" );
assert.eq( 3 , sumQuery( db.foo.find() ) , "sharded query 2" );

placeCheck( 5 );

// sort by num

assert.eq( 3 , sumQuery( db.foo.find().sort( { num : 1 } ) ) , "sharding query w/sort 1" );
assert.eq( 3 , sumQuery( db.foo.find().sort( { num : -1 } ) ) , "sharding query w/sort 2" );

assert.eq( "funny man" , db.foo.find().sort( { num : 1 } )[0].name , "sharding query w/sort 3 order wrong" );
assert.eq( -2 , db.foo.find().sort( { num : 1 } )[0].num , "sharding query w/sort 4 order wrong" );

assert.eq( "bob" , db.foo.find().sort( { num : -1 } )[0].name , "sharding query w/sort 5 order wrong" );
assert.eq( 3 , db.foo.find().sort( { num : -1 } )[0].num , "sharding query w/sort 6 order wrong" );

placeCheck( 6 );
// sory by name

function getNames( c ){
    return c.toArray().map( function(z){ return z.name; } );
}
correct = getNames( db.foo.find() ).sort();
assert.eq( correct , getNames( db.foo.find().sort( { name : 1 } ) ) );
correct = correct.reverse();
assert.eq( correct , getNames( db.foo.find().sort( { name : -1 } ) ) );

assert.eq( 3 , sumQuery( db.foo.find().sort( { name : 1 } ) ) , "sharding query w/non-shard sort 1" );
assert.eq( 3 , sumQuery( db.foo.find().sort( { name : -1 } ) ) , "sharding query w/non-shard sort 2" );


// sort by num multiple shards per server
s.adminCommand( { split : "test.foo" , middle : { num : 2 } } );
assert.eq( "funny man" , db.foo.find().sort( { num : 1 } )[0].name , "sharding query w/sort and another split 1 order wrong" );
assert.eq( "bob" , db.foo.find().sort( { num : -1 } )[0].name , "sharding query w/sort and another split 2 order wrong" );
assert.eq( "funny man" , db.foo.find( { num : { $lt : 100 } } ).sort( { num : 1 } ).arrayAccess(0).name , "sharding query w/sort and another split 3 order wrong" );

placeCheck( 7 );

// getMore
assert.eq( 4 , db.foo.find().limit(-4).toArray().length , "getMore 1" );
function countCursor( c ){
    var num = 0;
    while ( c.hasNext() ){
        c.next();
        num++;
    }
    return num;
}
assert.eq( 6 , countCursor( db.foo.find()._exec() ) , "getMore 2" );
assert.eq( 6 , countCursor( db.foo.find().limit(1)._exec() ) , "getMore 3" );

// update
person = db.foo.findOne( { num : 3 } );
assert.eq( "bob" , person.name , "update setup 1" );
person.name = "bob is gone";
db.foo.update( { num : 3 } , person );
person = db.foo.findOne( { num : 3 } );
assert.eq( "bob is gone" , person.name , "update test B" );

// remove
assert( db.foo.findOne( { num : 3 } ) != null , "remove test A" );
db.foo.remove( { num : 3 } );
assert.isnull( db.foo.findOne( { num : 3 } ) , "remove test B" );

db.foo.save( { num : 3 , name : "eliot2" } );
person = db.foo.findOne( { num : 3 } );
assert( person , "remove test C" );
assert.eq( person.name , "eliot2" );

db.foo.remove( { _id : person._id } );
assert.isnull( db.foo.findOne( { num : 3 } ) , "remove test E" );

placeCheck( 8 );

// TODO: getLastError
db.getLastError();
db.getPrevError();

// ---- move all to the secondary

assert.eq( 2 , s.onNumShards( "foo" ) , "on 2 shards" );

secondary.foo.insert( { num : -3 } );

s.adminCommand( { movechunk : "test.foo" , find : { num : -2 } , to : secondary.getMongo().name } );
assert.eq( 1 , s.onNumShards( "foo" ) , "on 1 shards" );

s.adminCommand( { movechunk : "test.foo" , find : { num : -2 } , to : primary.getMongo().name } );
assert.eq( 2 , s.onNumShards( "foo" ) , "on 2 shards again" );
assert.eq( 3 , s.config.chunks.count() , "only 3 chunks" );

s.stop();
