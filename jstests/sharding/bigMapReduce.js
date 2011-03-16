s = new ShardingTest( "bigMapReduce" , 2 , 1 , 1 , { chunksize : 1 } );

s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo", key : { "_id" : 1 } } )

db = s.getDB( "test" );
var str=""
for (i=0;i<4*1024;i++) { str=str+"a"; }
for (j=0; j<50; j++) for (i=0; i<512; i++){ db.foo.save({y:str})}
db.getLastError();

s.printChunks();
s.printChangeLog();

function map() { emit('count', 1); } 
function reduce(key, values) { return Array.sum(values) } 

gotAGoodOne = false;

for ( iter=0; iter<5; iter++ ){
    try {
        out = db.foo.mapReduce(map, reduce,"big_out") 
        gotAGoodOne = true
    }
    catch ( e ){
        if ( __mrerror__ && __mrerror__.cause && __mrerror__.cause.assertionCode == 13388 ){
            // TODO: SERVER-2396
            sleep( 1000 );
            continue;
        }
        printjson( __mrerror__ );
        throw e;
    }
}
assert( gotAGoodOne , "no good for basic" )

gotAGoodOne = false;
// test output to a different DB
// do it multiple times so that primary shard changes
for (iter = 0; iter < 5; iter++) {
    outCollStr = "mr_replace_col_" + iter;
    outDbStr = "mr_db_" + iter;

    print("Testing mr replace into DB " + iter)

    try {
        res = db.foo.mapReduce( map , reduce , { out : { replace: outCollStr, db: outDbStr } } )
        gotAGoodOne = true;
    }
    catch ( e ){
        if ( __mrerror__ && __mrerror__.cause && __mrerror__.cause.assertionCode == 13388 ){
            // TODO: SERVER-2396
            sleep( 1000 );
            continue;
        }
        printjson( __mrerror__ );
        throw e;
    }
    printjson(res);

    outDb = s.getDB(outDbStr);
    outColl = outDb[outCollStr];

    obj = outColl.convertToSingleObject("value");
    assert.eq( 25600 , obj.count , "Received wrong result " + obj.count );

    print("checking result field");
    assert.eq(res.result.collection, outCollStr, "Wrong collection " + res.result.collection);
    assert.eq(res.result.db, outDbStr, "Wrong db " + res.result.db);
}

assert( gotAGoodOne , "no good for out db" )

s.stop()

