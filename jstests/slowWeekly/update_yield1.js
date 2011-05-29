
t = db.update_yield1;
t.drop()

N = 10000;
i = 0;

while ( true ){
    function fill(){
        for ( ; i<N; i++ ){
            t.insert( { _id : i , n : 1 } )
        }
    }
    
     function timeUpdate(){
        return Date.timeFunc( 
            function(){
                t.update( {} , { $inc : { n : 1 } } , false , true );
                var r = db.getLastErrorObj();
            }
        );
        
    }
    
    fill();
    timeUpdate();
    timeUpdate();
    time = timeUpdate();
    print( N + "\t" + time );
    if ( time > 8000 )
        break;
    
    N *= 2;
}

// --- test 1

join = startParallelShell( "db.update_yield1.update( {} , { $inc : { n : 1 } } , false , true ); db.getLastError()" );

assert.soon( 
    function(){
        return db.currentOp().inprog.length > 0;
    } , "never doing update"
);

num = 0;
start = new Date();
while ( ( (new Date()).getTime() - start ) < ( time * 2 ) ){
    var me = Date.timeFunc( function(){ t.findOne(); } );
    if (me > 50) print("time: " + me);
    
    if ( num++ == 0 ){
        var x = db.currentOp()
        assert.eq( 1 , x.inprog.length , "nothing in prog" );
    }

    assert.gt( 2000 , me );
}

join();

var x = db.currentOp()
assert.eq( 0 , x.inprog.length , "weird 2" );

// --- test 2

join = startParallelShell( "db.update_yield1.update( { $atomic : true } , { $inc : { n : 1 } } , false , true ); db.getLastError()" );

sleep(1000); // wait for shell startup ops to finish

var x = db.currentOp();
printjson(x);
assert.eq(1, x.inprog.length, "never doing update 2");
assert.eq("update", x.inprog[0].op);

t.findOne(); // should wait for update to finish

var x = db.currentOp()
assert.eq( [] , x.inprog , "should have been atomic" );

join();
