numShards = 3
s = new ShardingTest( "parallel" , numShards , 2 , 2 , { sync : true } );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } ); 

db = s.getDB( "test" );

N = 10000;

for ( i=0; i<N; i+=(N/12) ) {
    s.adminCommand( { split : "test.foo" , middle : { _id : i } } )
    sh.moveChunk( "test.foo", { _id : i } , "shard000" + Math.floor( Math.random() * numShards ) )
}


for ( i=0; i<N; i++ )
    db.foo.insert( { _id : i } )
db.getLastError();


doCommand = function( dbname , cmd ) {
    x = benchRun( { ops : [ { op : "findOne" , ns : dbname + ".$cmd" , query : cmd } ] , 
                    host : db.getMongo().host , parallel : 2 , seconds : 2 } )
    printjson(x)
    x = benchRun( { ops : [ { op : "findOne" , ns : dbname + ".$cmd" , query : cmd } ] , 
                    host : s._mongos[1].host , parallel : 2 , seconds : 2 } )
    printjson(x)
}

doCommand( "test" , { dbstats : 1 } )
doCommand( "config" , { dbstats : 1 } )

x = s.getDB( "config" ).stats()
assert( x.ok , tojson(x) )
printjson(x)

s.stop()
