
// test repl basics
// data on master/slave is the same

var rt = new ReplTest( "basic1" );

m = rt.start( true );
s = rt.start( false );

function block(){
    am.runCommand( { getlasterror : 1 , w : 2 , wtimeout : 3000 } )
    sleep(3000); // 1.4 branch doesn't support w
}

function hash( db ){
    var s = "";
    var a = db.getCollectionNames();
    a = a.sort();
    a.forEach(
        function(cn){
            var c = db.getCollection( cn );
            s += cn + "\t" + c.find().count() + "\n";
            c.find().sort( { _id : 1 } ).forEach(
                function(o){
                    s += tojson( o , "" , true ) + "\n";
                }
            );
        }
    );
    return s;
}

am = m.getDB( "foo" );
as = s.getDB( "foo" );

function check( note ){
    var start = new Date();
    var x,y;
    while ( (new Date()).getTime() - start.getTime() < 30000 ){
        x = hash( am );
        y = hash( as );
        if ( x == y )
            return;
        sleep( 200 );
    }
    assert.eq( x , y , note );
}

am.a.save( { x : 1 } );
check( "A" );

am.a.save( { x : 5 } );

am.a.update( {} , { $inc : { x : 1 } } );
check( "B" );

am.a.update( {} , { $inc : { x : 1 } } , false , true );
check( "C" );

// -----   check features -------

// map/reduce
am.mr.insert( { tags : [ "a" ] } )
am.mr.insert( { tags : [ "a" , "b" ] } )
am.getLastError();
check( "mr setup" );

m = function(){
    for ( var i=0; i<this.tags.length; i++ ){
        print( "\t " + i );
        emit( this.tags[i] , 1 );
    }
}

r = function( key , v ){
    return Array.sum( v );
}

correct = { a : 2 , b : 1 };

function checkMR( t ){
    var res = t.mapReduce( m , r );
    assert.eq( correct , res.convertToSingleObject() , "checkMR: " + tojson( t ) );
}

function checkNumCollections( msg , diff ){
    if ( ! diff ) diff = 0;
    var m = am.getCollectionNames();
    var s = as.getCollectionNames();
    assert.eq( m.length + diff , s.length , "lengths bad \n" + tojson( m ) + "\n" + tojson( s ) );
}

checkNumCollections( "MR1" );
checkMR( am.mr );
checkMR( as.mr );
checkNumCollections( "MR2" );

block();
checkNumCollections( "MR3" );

var res = am.mr.mapReduce( m , r , { out : "xyz" } );
block();

checkNumCollections( "MR4" );


t = am.rpos;
t.insert( { _id : 1 , a : [ { n : "a" , c : 1 } , { n : "b" , c : 1 } , { n : "c" , c : 1 } ] , b : [ 1 , 2 , 3 ] } )
block();
check( "after pos 1 " );

t.update( { "a.n" : "b" } , { $inc : { "a.$.c" : 1 } } )
block();
check( "after pos 2 " );

t.update( { "b" : 2 } , { $inc : { "b.$" : 1 } } )
block();
check( "after pos 3 " );

t.update( { "b" : 3} , { $set : { "b.$" : 17 } } )
block();
check( "after pos 4 " );


printjson( am.rpos.findOne() )
printjson( as.rpos.findOne() )

//am.getSisterDB( "local" ).getCollection( "oplog.$main" ).find().limit(10).sort( { $natural : -1 } ).forEach( printjson )

rt.stop();




