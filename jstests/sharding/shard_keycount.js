// Tests splitting a chunk twice
(function() {

var s = new ShardingTest({ name: "shard_keycount",
                           shards: 2,
                           mongos: 1,
                           other:{ chunkSize: 1 } });

dbName = "test"
collName = "foo"
ns = dbName + "." + collName
	
db = s.getDB( dbName );

for(var i = 0; i < 10; i++){
	db.foo.insert({ _id : i })
}

// Enable sharding on DB
s.adminCommand( { enablesharding : dbName } );
s.ensurePrimaryShard(dbName, 'shard0001');

// Enable sharding on collection
s.adminCommand( { shardcollection : ns, key : { _id : 1 } } );


// Split into two chunks
s.adminCommand({ split : ns, find : { _id : 3 } })

coll = db.getCollection( collName )

// Split chunk again
s.adminCommand({ split : ns, find : { _id : 3 } })

coll.update({ _id : 3 }, { _id : 3 })

// Split chunk again
s.adminCommand({ split : ns, find : { _id : 3 } })

coll.update({ _id : 3 }, { _id : 3 })

// Split chunk again
// FAILS since the key count is based on the full index, not the chunk itself
// i.e. Split point calc'd is 5 key offset (10 documents), but only four docs
// in chunk with bounds _id : 0 => 5
s.adminCommand({ split : ns, find : { _id : 3 } })

s.stop();

});
