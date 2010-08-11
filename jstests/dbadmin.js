
t = db.dbadmin;
t.save( { x : 1 } );

before = db._adminCommand( "serverStatus" )
if ( before.mem.supported ){
    cmdres = db._adminCommand( "closeAllDatabases" );
    after = db._adminCommand( "serverStatus" );
    assert( before.mem.mapped > after.mem.mapped , "closeAllDatabases does something before:" + tojson( before.mem ) + " after:" + tojson( after.mem ) + " cmd res:" + tojson( cmdres ) );
    print( before.mem.mapped + " -->> " + after.mem.mapped );
}
else {
    print( "can't test serverStatus on this machine" );
}

t.save( { x : 1 } );

res = db._adminCommand( "listDatabases" );
assert( res.databases && res.databases.length > 0 , "listDatabases 1 " + tojson(res) );

x = db._adminCommand( "ismaster" );
assert( x.ismaster , "ismaster failed: " + tojson( x ) )

// TODO: add more tests here
