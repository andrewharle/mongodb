// version2.js

s = new ShardingTest( "version2" , 1 , 2 )

a = s._connections[0].getDB( "admin" );

// setup from one client

assert( a.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).mine.i == 0 );
assert( a.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).global.i == 0 );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 2 , authoritative : true } ).ok == 1 );

assert( a.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).mine.i == 2 );
assert( a.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).global.i == 2 );

// from another client

a2 = connect( s._connections[0].name + "/admin" );

assert.eq( a2.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).global.i , 2 , "a2 global 1" );
assert.eq( a2.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).mine.i , 0 , "a2 mine 1" );

function simpleFindOne(){
    return a2.getMongo().getDB( "alleyinsider" ).foo.findOne();
}

assert.commandWorked( a2.runCommand( { "setShardVersion" : "alleyinsider.bar" , configdb : s._configDB , version : 2 , authoritative : true } ) , "setShardVersion bar temp");
assert.throws( simpleFindOne , [] , "should complain about not in sharded mode 1" );
assert( a2.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 2 } ).ok == 1 , "setShardVersion a2-1");
simpleFindOne(); // now should run ok
assert( a2.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 3 } ).ok == 1 , "setShardVersion a2-2");
simpleFindOne(); // newer version is ok


s.stop();
