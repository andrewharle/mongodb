// Test resync command

soonCount = function( count ) {
    assert.soon( function() { 
//                print( "check count" );
//                print( "count: " + s.getDB( baseName ).z.find().count() );
                return s.getDB("foo").a.find().count() == count; 
                } );    
}

doTest = function( signal ) {
    
    var rt = new ReplTest( "repl2tests" );

    // spec small oplog to make slave get out of sync
    m = rt.start( true , { oplogSize : "1" } );
    s = rt.start( false );
    
    am = m.getDB("foo").a
    
    am.save( { _id: new ObjectId() } );
    soonCount( 1 );
    assert.eq( 0, s.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok );
    rt.stop( false , signal );
    
    big = new Array( 2000 ).toString();
    for( i = 0; i < 1000; ++i )
        am.save( { _id: new ObjectId(), i: i, b: big } );

    s = rt.start( false , null , true );
    assert.soon( function() { return 1 == s.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok; } );

    soonCount( 1001 );
    as = s.getDB("foo").a
    assert.eq( 1, as.find( { i: 0 } ).count() );
    assert.eq( 1, as.find( { i: 999 } ).count() );
    
    assert.eq( 0, s.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok );

    rt.stop();

}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
