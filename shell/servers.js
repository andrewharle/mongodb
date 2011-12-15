_parsePath = function() {
    var dbpath = "";
    for( var i = 0; i < arguments.length; ++i )
        if ( arguments[ i ] == "--dbpath" )
            dbpath = arguments[ i + 1 ];

    if ( dbpath == "" )
        throw "No dbpath specified";

    return dbpath;
}

_parsePort = function() {
    var port = "";
    for( var i = 0; i < arguments.length; ++i )
        if ( arguments[ i ] == "--port" )
            port = arguments[ i + 1 ];

    if ( port == "" )
        throw "No port specified";
    return port;
}

connectionURLTheSame = function( a , b ){
    if ( a == b )
        return true;

    if ( ! a || ! b )
        return false;

    a = a.split( "/" )[0]
    b = b.split( "/" )[0]

    return a == b;
}

assert( connectionURLTheSame( "foo" , "foo" ) )
assert( ! connectionURLTheSame( "foo" , "bar" ) )

assert( connectionURLTheSame( "foo/a,b" , "foo/b,a" ) )
assert( ! connectionURLTheSame( "foo/a,b" , "bar/a,b" ) )

createMongoArgs = function( binaryName , args ){
    var fullArgs = [ binaryName ];

    if ( args.length == 1 && isObject( args[0] ) ){
        var o = args[0];
        for ( var k in o ){
          if ( o.hasOwnProperty(k) ){
            if ( k == "v" && isNumber( o[k] ) ){
                var n = o[k];
                if ( n > 0 ){
                    if ( n > 10 ) n = 10;
                    var temp = "-";
                    while ( n-- > 0 ) temp += "v";
                    fullArgs.push( temp );
                }
            }
            else {
                fullArgs.push( "--" + k );
                if ( o[k] != "" )
                    fullArgs.push( "" + o[k] );
            }
          }
        }
    }
    else {
        for ( var i=0; i<args.length; i++ )
            fullArgs.push( args[i] )
    }

    return fullArgs;
}

__nextPort = 27000;
startMongodTest = function (port, dirname, restart, extraOptions ) {
    if (!port)
        port = __nextPort++;
    var f = startMongodEmpty;
    if (restart)
        f = startMongodNoReset;
    if (!dirname)
        dirname = "" + port; // e.g., data/db/27000

    var useHostname = false;
    if (extraOptions) {
         useHostname = extraOptions.useHostname;
         delete extraOptions.useHostname;
    }

    
    var options = 
        {
            port: port,
            dbpath: "/data/db/" + dirname,
            noprealloc: "",
            smallfiles: "",
            oplogSize: "40",
            nohttpinterface: ""
        };
    
    if( jsTestOptions().noJournal ) options["nojournal"] = ""
    if( jsTestOptions().noJournalPrealloc ) options["nopreallocj"] = ""

    if ( extraOptions )
        Object.extend( options , extraOptions );
    
    var conn = f.apply(null, [ options ] );

    conn.name = (useHostname ? getHostName() : "localhost") + ":" + port;
    return conn;
}

// Start a mongod instance and return a 'Mongo' object connected to it.
// This function's arguments are passed as command line arguments to mongod.
// The specified 'dbpath' is cleared if it exists, created if not.
// var conn = startMongodEmpty("--port", 30000, "--dbpath", "asdf");
startMongodEmpty = function () {
    var args = createMongoArgs("mongod", arguments);

    var dbpath = _parsePath.apply(null, args);
    resetDbpath(dbpath);

    return startMongoProgram.apply(null, args);
}
startMongod = function () {
    print("startMongod WARNING DELETES DATA DIRECTORY THIS IS FOR TESTING ONLY");
    return startMongodEmpty.apply(null, arguments);
}
startMongodNoReset = function(){
    var args = createMongoArgs( "mongod" , arguments );
    return startMongoProgram.apply( null, args );
}

startMongos = function(){
    return startMongoProgram.apply( null, createMongoArgs( "mongos" , arguments ) );
}

/* Start mongod or mongos and return a Mongo() object connected to there.
  This function's first argument is "mongod" or "mongos" program name, \
  and subsequent arguments to this function are passed as
  command line arguments to the program.
*/
startMongoProgram = function(){
    var port = _parsePort.apply( null, arguments );

    _startMongoProgram.apply( null, arguments );

    var m;
    assert.soon
    ( function() {
        try {
            m = new Mongo( "127.0.0.1:" + port );
            return true;
        } catch( e ) {
        }
        return false;
    }, "unable to connect to mongo program on port " + port, 600 * 1000 );

    return m;
}

// Start a mongo program instance.  This function's first argument is the
// program name, and subsequent arguments to this function are passed as
// command line arguments to the program.  Returns pid of the spawned program.
startMongoProgramNoConnect = function() {
    return _startMongoProgram.apply( null, arguments );
}

myPort = function() {
    var m = db.getMongo();
    if ( m.host.match( /:/ ) )
        return m.host.match( /:(.*)/ )[ 1 ];
    else
        return 27017;
}

/**
 * otherParams can be:
 * * useHostname to use the hostname (instead of localhost)
 */
ShardingTest = function( testName , numShards , verboseLevel , numMongos , otherParams ){
    
    // Check if testName is an object, if so, pull params from there
    var keyFile = undefined
    if( testName && ! testName.charAt ){
        var params = testName
        testName = params.name || "test"
        numShards = params.shards || 2
        verboseLevel = params.verbose || 0
        numMongos = params.mongos || 1
        otherParams = params.other || {}
        keyFile = params.keyFile || otherParams.keyFile
    }
    
    this._testName = testName;

    if ( ! otherParams )
        otherParams = {}
    this._connections = [];
    
    if ( otherParams.sync && numShards < 3 )
        throw "if you want sync, you need at least 3 servers";

    var localhost = otherParams.useHostname ? getHostName() : "localhost";

    this._alldbpaths = []
    
    if ( otherParams.rs ){
        localhost = getHostName();
        // start replica sets
        this._rs = []
        for ( var i=0; i<numShards; i++){
            var setName = testName + "-rs" + i;
            
            var rsDefaults = { oplogSize : 40, nodes : 3 }
            var rsParams = otherParams["rs" + i]
            
            for( var param in rsParams ){
                rsDefaults[param] = rsParams[param]
            }

            var numReplicas = rsDefaults.nodes || otherParams.numReplicas || 3
            delete rsDefaults.nodes 
            
            var rs = new ReplSetTest( { name : setName , nodes : numReplicas , startPort : 31100 + ( i * 100 ), keyFile : keyFile } );
            this._rs[i] = { setName : setName , test : rs , nodes : rs.startSet( rsDefaults ) , url : rs.getURL() };
            rs.initiate();
            
        }

        for ( var i=0; i<numShards; i++){
            var rs = this._rs[i].test;
            rs.getMaster().getDB( "admin" ).foo.save( { x : 1 } )
            rs.awaitReplication();
	    var xxx = new Mongo( rs.getURL() );
	    xxx.name = rs.getURL();
            this._connections.push( xxx )
            this["shard" + i] = xxx
        }
        
        this._configServers = []
        for ( var i=0; i<3; i++ ){            
            var options = otherParams.extraOptions
            if( keyFile ) options["keyFile"] = keyFile
            var conn = startMongodTest( 30000 + i , testName + "-config" + i, false, options );
            this._alldbpaths.push( testName + "-config" + i )
            this._configServers.push( conn );
        }

        this._configDB = localhost + ":30000," + localhost + ":30001," + localhost + ":30002";
        this._configConnection = new Mongo( this._configDB );
        if (!otherParams.noChunkSize) {
            this._configConnection.getDB( "config" ).settings.insert( { _id : "chunksize" , value : otherParams.chunksize || 50 } );
        }
    }
    else {
        for ( var i=0; i<numShards; i++){
            var options = { useHostname : otherParams.useHostname }
            if( keyFile ) options["keyFile"] = keyFile
            var conn = startMongodTest( 30000 + i , testName + i, 0, options );
            this._alldbpaths.push( testName +i )
            this._connections.push( conn );
            this["shard" + i] = conn
        }

        if ( otherParams.sync ){
            this._configDB = localhost+":30000,"+localhost+":30001,"+localhost+":30002";
            this._configConnection = new Mongo( this._configDB );
            this._configConnection.getDB( "config" ).settings.insert( { _id : "chunksize" , value : otherParams.chunksize || 50 } );        
        }
        else {
            this._configDB = localhost + ":30000";
            this._connections[0].getDB( "config" ).settings.insert( { _id : "chunksize" , value : otherParams.chunksize || 50 } );
        }
    }
    
    this._mongos = [];
    var startMongosPort = 31000;
    for ( var i=0; i<(numMongos||1); i++ ){
        var myPort =  startMongosPort - i;
        print("ShardingTest config: "+this._configDB);
        var opts = { port : startMongosPort - i , v : verboseLevel || 0 , configdb : this._configDB };
        if( keyFile ) opts["keyFile"] = keyFile
        for (var j in otherParams.extraOptions) {
            opts[j] = otherParams.extraOptions[j];
        }
        var conn = startMongos( opts );
        conn.name = localhost + ":" + myPort;
        this._mongos.push( conn );
        if ( i == 0 ) {
            this.s = conn;
        }
        this["s" + i] = conn
    }

    var admin = this.admin = this.s.getDB( "admin" );
    this.config = this.s.getDB( "config" );

    if ( ! otherParams.manualAddShard ){
        this._connections.forEach(
            function(z){
                var n = z.name;
                if ( ! n ){
                    n = z.host;
                    if ( ! n )
                        n = z;
                }
                print( "ShardingTest going to add shard: " + n )
                x = admin.runCommand( { addshard : n } );
                printjson( x )
            }
        );
    }
}

ShardingTest.prototype.getRSEntry = function( setName ){
    for ( var i=0; i<this._rs.length; i++ )
        if ( this._rs[i].setName == setName )
            return this._rs[i];
    throw "can't find rs: " + setName;
}

ShardingTest.prototype.getDB = function( name ){
    return this.s.getDB( name );
}

ShardingTest.prototype.getServerName = function( dbname ){
    var x = this.config.databases.findOne( { _id : "" + dbname } );
    if ( x )
        return x.primary;
    this.config.databases.find().forEach( printjson );
    throw "couldn't find dbname: " + dbname + " total: " + this.config.databases.count();
}


ShardingTest.prototype.getNonPrimaries = function( dbname ){
    var x = this.config.databases.findOne( { _id : dbname } );
    if ( ! x ){
        this.config.databases.find().forEach( printjson );
        throw "couldn't find dbname: " + dbname + " total: " + this.config.databases.count();
    }
    
    return this.config.shards.find( { _id : { $ne : x.primary } } ).map( function(z){ return z._id; } )
}


ShardingTest.prototype.getConnNames = function(){
    var names = [];
    for ( var i=0; i<this._connections.length; i++ ){
        names.push( this._connections[i].name );
    }
    return names; 
}

ShardingTest.prototype.getServer = function( dbname ){
    var name = this.getServerName( dbname );

    var x = this.config.shards.findOne( { _id : name } );
    if ( x )
        name = x.host;

    var rsName = null;
    if ( name.indexOf( "/" ) > 0 )
	rsName = name.substring( 0 , name.indexOf( "/" ) );
    
    for ( var i=0; i<this._connections.length; i++ ){
        var c = this._connections[i];
        if ( connectionURLTheSame( name , c.name ) || 
             connectionURLTheSame( rsName , c.name ) )
            return c;
    }
    
    throw "can't find server for: " + dbname + " name:" + name;

}

ShardingTest.prototype.normalize = function( x ){
    var z = this.config.shards.findOne( { host : x } );
    if ( z )
        return z._id;
    return x;
}

ShardingTest.prototype.getOther = function( one ){
    if ( this._connections.length < 2 )
        throw "getOther only works with 2 servers";

    if ( one._mongo )
        one = one._mongo
    
    for( var i = 0; i < this._connections.length; i++ ){
        if( this._connections[i] != one ) return this._connections[i]
    }
    
    return null
}

ShardingTest.prototype.getAnother = function( one ){
    if(this._connections.length < 2)
    	throw "getAnother() only works with multiple servers";
	
	if ( one._mongo )
        one = one._mongo
    
    for(var i = 0; i < this._connections.length; i++){
    	if(this._connections[i] == one)
    		return this._connections[(i + 1) % this._connections.length];
    }
}

ShardingTest.prototype.getFirstOther = function( one ){
    for ( var i=0; i<this._connections.length; i++ ){
        if ( this._connections[i] != one )
        return this._connections[i];
    }
    throw "impossible";
}

ShardingTest.prototype.stop = function(){
    for ( var i=0; i<this._mongos.length; i++ ){
        stopMongoProgram( 31000 - i );
    }
    for ( var i=0; i<this._connections.length; i++){
        stopMongod( 30000 + i );
    }
    if ( this._rs ){
        for ( var i=0; i<this._rs.length; i++ ){
            this._rs[i].test.stopSet( 15 );
        }
    }
    if ( this._alldbpaths ){
        for( i=0; i<this._alldbpaths.length; i++ ){
            resetDbpath( "/data/db/" + this._alldbpaths[i] );
        }
    }

    print('*** ShardingTest ' + this._testName + " completed successfully ***");
}

ShardingTest.prototype.adminCommand = function(cmd){
    var res = this.admin.runCommand( cmd );
    if ( res && res.ok == 1 )
        return true;

    throw "command " + tojson( cmd ) + " failed: " + tojson( res );
}

ShardingTest.prototype._rangeToString = function(r){
    return tojsononeline( r.min ) + " -> " + tojsononeline( r.max );
}

ShardingTest.prototype.printChangeLog = function(){
    var s = this;
    this.config.changelog.find().forEach( 
        function(z){
            var msg = z.server + "\t" + z.time + "\t" + z.what;
            for ( i=z.what.length; i<15; i++ )
                msg += " ";
            msg += " " + z.ns + "\t";
            if ( z.what == "split" ){
                msg += s._rangeToString( z.details.before ) + " -->> (" + s._rangeToString( z.details.left ) + "),(" + s._rangeToString( z.details.right ) + ")";
            }
            else if (z.what == "multi-split" ){
                msg += s._rangeToString( z.details.before ) + "  -->> (" + z.details.number + "/" + z.details.of + " " + s._rangeToString( z.details.chunk ) + ")"; 
            }
            else {
                msg += tojsononeline( z.details );
            }

            print( "ShardingTest " + msg )
        }
    );

}

ShardingTest.prototype.getChunksString = function( ns ){
    var q = {}
    if ( ns )
        q.ns = ns;

    var s = "";
    this.config.chunks.find( q ).sort( { ns : 1 , min : 1 } ).forEach( 
        function(z){
            s +=  "  " + z._id + "\t" + z.lastmod.t + "|" + z.lastmod.i + "\t" + tojson(z.min) + " -> " + tojson(z.max) + " " + z.shard + "  " + z.ns + "\n";
        }
    );
    
    return s;
}

ShardingTest.prototype.printChunks = function( ns ){
    print( "ShardingTest " + this.getChunksString( ns ) );
}

ShardingTest.prototype.printShardingStatus = function(){
    printShardingStatus( this.config );
}

ShardingTest.prototype.printCollectionInfo = function( ns , msg ){
    var out = "";
    if ( msg )
        out += msg + "\n";
    out += "sharding collection info: " + ns + "\n";
    for ( var i=0; i<this._connections.length; i++ ){
        var c = this._connections[i];
        out += "  mongod " + c + " " + tojson( c.getCollection( ns ).getShardVersion() , " " , true ) + "\n";
    }
    for ( var i=0; i<this._mongos.length; i++ ){
        var c = this._mongos[i];
        out += "  mongos " + c + " " + tojson( c.getCollection( ns ).getShardVersion() , " " , true ) + "\n";
    }
    
    out += this.getChunksString( ns );

    print( "ShardingTest " + out );
}

printShardingStatus = function( configDB , verbose ){
    if (configDB === undefined)
        configDB = db.getSisterDB('config')
    
    var version = configDB.getCollection( "version" ).findOne();
    if ( version == null ){
        print( "printShardingStatus: not a shard db!" );
        return;
    }
    
    var raw = "";
    var output = function(s){
        raw += s + "\n";
    }
    output( "--- Sharding Status --- " );
    output( "  sharding version: " + tojson( configDB.getCollection( "version" ).findOne() ) );
    
    output( "  shards:" );
    configDB.shards.find().sort( { _id : 1 } ).forEach( 
        function(z){
            output( "\t" + tojsononeline( z ) );
        }
    );

    output( "  databases:" );
    configDB.databases.find().sort( { name : 1 } ).forEach( 
        function(db){
            output( "\t" + tojsononeline(db,"",true) );
        
            if (db.partitioned){
                configDB.collections.find( { _id : new RegExp( "^" + db._id + "\\." ) } ).sort( { _id : 1 } ).forEach(
                    function( coll ){
                        if ( coll.dropped == false ){
                            output("\t\t" + coll._id + " chunks:");
                            
                            res = configDB.chunks.group( { cond : { ns : coll._id } , key : { shard : 1 }  , reduce : function( doc , out ){ out.nChunks++; } , initial : { nChunks : 0 } } );
                            var totalChunks = 0;
                            res.forEach( function(z){
                                totalChunks += z.nChunks;
                                output( "\t\t\t\t" + z.shard + "\t" + z.nChunks );
                            } )
                            
                            if ( totalChunks < 20 || verbose ){
                                configDB.chunks.find( { "ns" : coll._id } ).sort( { min : 1 } ).forEach( 
                                    function(chunk){
                                        output( "\t\t\t" + tojson( chunk.min ) + " -->> " + tojson( chunk.max ) + 
                                                " on : " + chunk.shard + " " + tojson( chunk.lastmod ) );
                                    }
                                );
                            }
                            else {
                                output( "\t\t\ttoo many chunks to print, use verbose if you want to force print" );
                            }
                        }
                    }
                )
            }
        }
    );
    
    print( raw );
}

printShardingSizes = function(){
    configDB = db.getSisterDB('config')
    
    var version = configDB.getCollection( "version" ).findOne();
    if ( version == null ){
        print( "printShardingSizes : not a shard db!" );
        return;
    }
    
    var raw = "";
    var output = function(s){
        raw += s + "\n";
    }
    output( "--- Sharding Status --- " );
    output( "  sharding version: " + tojson( configDB.getCollection( "version" ).findOne() ) );
    
    output( "  shards:" );
    var shards = {};
    configDB.shards.find().forEach( 
        function(z){
            shards[z._id] = new Mongo(z.host);
            output( "      " + tojson(z) );
        }
    );

    var saveDB = db;
    output( "  databases:" );
    configDB.databases.find().sort( { name : 1 } ).forEach( 
        function(db){
            output( "\t" + tojson(db,"",true) );
        
            if (db.partitioned){
                configDB.collections.find( { _id : new RegExp( "^" + db._id + "\." ) } ).sort( { _id : 1 } ).forEach(
                    function( coll ){
                        output("\t\t" + coll._id + " chunks:");
                        configDB.chunks.find( { "ns" : coll._id } ).sort( { min : 1 } ).forEach( 
                            function(chunk){
                                var mydb = shards[chunk.shard].getDB(db._id)
                                var out = mydb.runCommand({dataSize: coll._id,
                                                           keyPattern: coll.key, 
                                                           min: chunk.min,
                                                           max: chunk.max });
                                delete out.millis;
                                delete out.ok;

                                output( "\t\t\t" + tojson( chunk.min ) + " -->> " + tojson( chunk.max ) + 
                                        " on : " + chunk.shard + " " + tojson( out ) );

                            }
                        );
                    }
                )
            }
        }
    );
    
    print( raw );
}

ShardingTest.prototype.sync = function(){
    this.adminCommand( "connpoolsync" );
}

ShardingTest.prototype.onNumShards = function( collName , dbName ){
    this.sync(); // we should sync since we're going directly to mongod here
    dbName = dbName || "test";
    var num=0;
    for ( var i=0; i<this._connections.length; i++ )
        if ( this._connections[i].getDB( dbName ).getCollection( collName ).count() > 0 )
            num++;
    return num;
}


ShardingTest.prototype.shardCounts = function( collName , dbName ){
    this.sync(); // we should sync since we're going directly to mongod here
    dbName = dbName || "test";
    var counts = {}
    for ( var i=0; i<this._connections.length; i++ )
        counts[i] = this._connections[i].getDB( dbName ).getCollection( collName ).count();
    return counts;
}

ShardingTest.prototype.chunkCounts = function( collName , dbName ){
    dbName = dbName || "test";
    var x = {}

    s.config.shards.find().forEach( 
        function(z){
            x[z._id] = 0;
        }
    );
    
    s.config.chunks.find( { ns : dbName + "." + collName } ).forEach(
        function(z){
            if ( x[z.shard] )
                x[z.shard]++
            else
                x[z.shard] = 1;
        }
    );
    return x;

}

ShardingTest.prototype.chunkDiff = function( collName , dbName ){
    var c = this.chunkCounts( collName , dbName );
    var min = 100000000;
    var max = 0;
    for ( var s in c ){
        if ( c[s] < min )
            min = c[s];
        if ( c[s] > max )
            max = c[s];
    }
    print( "ShardingTest input: " + tojson( c ) + " min: " + min + " max: " + max  );
    return max - min;
}

ShardingTest.prototype.getShard = function( coll, query ){
    var shards = this.getShards( coll, query )
    assert.eq( shards.length, 1 )
    return shards[0]
}

// Returns the shards on which documents matching a particular query reside
ShardingTest.prototype.getShards = function( coll, query ){
    if( ! coll.getDB )
        coll = this.s.getCollection( coll )
    
    var explain = coll.find( query ).explain()
    
    var shards = []
        
    if( explain.shards ){
        
        for( var shardName in explain.shards ){           
            for( var i = 0; i < explain.shards[shardName].length; i++ ){
                if( explain.shards[shardName][i].n && explain.shards[shardName][i].n > 0 )
                    shards.push( shardName )
            }
        }
        
    }
    
    for( var i = 0; i < shards.length; i++ ){
        for( var j = 0; j < this._connections.length; j++ ){
            if ( connectionURLTheSame(  this._connections[j].name , shards[i] ) ){
                shards[i] = this._connections[j]
                break;
            }
        }
    }
    
    return shards
}

ShardingTest.prototype.isSharded = function( collName ){
    
    var collName = "" + collName
    var dbName = undefined
    
    if( typeof collName.getCollectionNames == 'function' ){
        dbName = "" + collName
        collName = undefined
    }
    
    if( dbName ){
        var x = this.config.databases.findOne( { _id : dbname } )
        if( x ) return x.partitioned
        else return false
    }
    
    if( collName ){
        var x = this.config.collections.findOne( { _id : collName } )
        if( x ) return true
        else return false
    }
    
}

ShardingTest.prototype.shardGo = function( collName , key , split , move , dbName ){
    
    split = ( split != false ? ( split || key ) : split )
    move = ( split != false && move != false ? ( move || split ) : false )
    
    if( collName.getDB )
        dbName = "" + collName.getDB()
    else dbName = dbName || "test";

    var c = dbName + "." + collName;
    if( collName.getDB )
        c = "" + collName

    var isEmpty = this.s.getCollection( c ).count() == 0
        
    if( ! this.isSharded( dbName ) )
        this.s.adminCommand( { enableSharding : dbName } )
    
    var result = this.s.adminCommand( { shardcollection : c , key : key } )
    if( ! result.ok ){
        printjson( result )
        assert( false )
    }
    
    if( split == false ) return
    
    result = this.s.adminCommand( { split : c , middle : split } );
    if( ! result.ok ){
        printjson( result )
        assert( false )
    }
        
    if( move == false ) return
    
    var result = null
    for( var i = 0; i < 5; i++ ){
        result = this.s.adminCommand( { movechunk : c , find : move , to : this.getOther( this.getServer( dbName ) ).name } );
        if( result.ok ) break;
        sleep( 5 * 1000 );
    }
    printjson( result )
    assert( result.ok )
    
};

ShardingTest.prototype.shardColl = ShardingTest.prototype.shardGo

ShardingTest.prototype.setBalancer = function( balancer ){
    if( balancer || balancer == undefined ){
        this.config.settings.update( { _id: "balancer" }, { $set : { stopped: false } } , true )
    }
    else if( balancer == false ){
        this.config.settings.update( { _id: "balancer" }, { $set : { stopped: true } } , true )
    }
}

/**
 * Run a mongod process.
 *
 * After initializing a MongodRunner, you must call start() on it.
 * @param {int} port port to run db on, use allocatePorts(num) to requision
 * @param {string} dbpath path to use
 * @param {boolean} peer pass in false (DEPRECATED, was used for replica pair host)
 * @param {boolean} arbiter pass in false (DEPRECATED, was used for replica pair host)
 * @param {array} extraArgs other arguments for the command line
 * @param {object} options other options include no_bind to not bind_ip to 127.0.0.1
 *    (necessary for replica set testing)
 */
MongodRunner = function( port, dbpath, peer, arbiter, extraArgs, options ) {
    this.port_ = port;
    this.dbpath_ = dbpath;
    this.peer_ = peer;
    this.arbiter_ = arbiter;
    this.extraArgs_ = extraArgs;
    this.options_ = options ? options : {};
};

/**
 * Start this mongod process.
 *
 * @param {boolean} reuseData If the data directory should be left intact (default is to wipe it)
 */
MongodRunner.prototype.start = function( reuseData ) {
    var args = [];
    if ( reuseData ) {
        args.push( "mongod" );
    }
    args.push( "--port" );
    args.push( this.port_ );
    args.push( "--dbpath" );
    args.push( this.dbpath_ );
    args.push( "--nohttpinterface" );
    args.push( "--noprealloc" );
    args.push( "--smallfiles" );
    if (!this.options_.no_bind) {
      args.push( "--bind_ip" );
      args.push( "127.0.0.1" );
    }
    if ( this.extraArgs_ ) {
        args = args.concat( this.extraArgs_ );
    }
    removeFile( this.dbpath_ + "/mongod.lock" );
    if ( reuseData ) {
        return startMongoProgram.apply( null, args );
    } else {
        return startMongod.apply( null, args );
    }
}

MongodRunner.prototype.port = function() { return this.port_; }

MongodRunner.prototype.toString = function() { return [ this.port_, this.dbpath_, this.peer_, this.arbiter_ ].toString(); }

ToolTest = function( name ){
    this.name = name;
    this.port = allocatePorts(1)[0];
    this.baseName = "jstests_tool_" + name;
    this.root = "/data/db/" + this.baseName;
    this.dbpath = this.root + "/";
    this.ext = this.root + "_external/";
    this.extFile = this.root + "_external/a";
    resetDbpath( this.dbpath );
}

ToolTest.prototype.startDB = function( coll ){
    assert( ! this.m , "db already running" );
 
    this.m = startMongoProgram( "mongod" , "--port", this.port , "--dbpath" , this.dbpath , "--nohttpinterface", "--noprealloc" , "--smallfiles" , "--bind_ip", "127.0.0.1" );
    this.db = this.m.getDB( this.baseName );
    if ( coll )
        return this.db.getCollection( coll );
    return this.db;
}

ToolTest.prototype.stop = function(){
    if ( ! this.m )
        return;
    stopMongod( this.port );
    this.m = null;
    this.db = null;

    print('*** ' + this.name + " completed successfully ***");
}

ToolTest.prototype.runTool = function(){
    var a = [ "mongo" + arguments[0] ];

    var hasdbpath = false;
    
    for ( var i=1; i<arguments.length; i++ ){
        a.push( arguments[i] );
        if ( arguments[i] == "--dbpath" )
            hasdbpath = true;
    }

    if ( ! hasdbpath ){
        a.push( "--host" );
        a.push( "127.0.0.1:" + this.port );
    }

    return runMongoProgram.apply( null , a );
}


ReplTest = function( name, ports ){
    this.name = name;
    this.ports = ports || allocatePorts( 2 );
}

ReplTest.prototype.getPort = function( master ){
    if ( master )
        return this.ports[ 0 ];
    return this.ports[ 1 ]
}

ReplTest.prototype.getPath = function( master ){
    var p = "/data/db/" + this.name + "-";
    if ( master )
        p += "master";
    else
        p += "slave"
    return p;
}

ReplTest.prototype.getOptions = function( master , extra , putBinaryFirst, norepl ){

    if ( ! extra )
        extra = {};

    if ( ! extra.oplogSize )
        extra.oplogSize = "40";
        
    var a = []
    if ( putBinaryFirst )
        a.push( "mongod" )
    a.push( "--nohttpinterface", "--noprealloc", "--bind_ip" , "127.0.0.1" , "--smallfiles" );

    a.push( "--port" );
    a.push( this.getPort( master ) );

    a.push( "--dbpath" );
    a.push( this.getPath( master ) );
    
    if( jsTestOptions().noJournal ) a.push( "--nojournal" )
    if( jsTestOptions().noJournalPrealloc ) a.push( "--nopreallocj" )

    if ( !norepl ) {
        if ( master ){
            a.push( "--master" );
        }
        else {
            a.push( "--slave" );
            a.push( "--source" );
            a.push( "127.0.0.1:" + this.ports[0] );
        }
    }
    
    for ( var k in extra ){
        var v = extra[k];
        a.push( "--" + k );
        if ( v != null )
            a.push( v );                    
    }

    return a;
}

ReplTest.prototype.start = function( master , options , restart, norepl ){
    var lockFile = this.getPath( master ) + "/mongod.lock";
    removeFile( lockFile );
    var o = this.getOptions( master , options , restart, norepl );


    if ( restart )
        return startMongoProgram.apply( null , o );
    else
        return startMongod.apply( null , o );
}

ReplTest.prototype.stop = function( master , signal ){
    if ( arguments.length == 0 ){
        this.stop( true );
        this.stop( false );
        return;
    }

    print('*** ' + this.name + " completed successfully ***");
    return stopMongod( this.getPort( master ) , signal || 15 );
}

allocatePorts = function( n , startPort ) {
    var ret = [];
    var start = startPort || 31000;
    for( var i = start; i < start + n; ++i )
        ret.push( i );
    return ret;
}


SyncCCTest = function( testName , extraMongodOptions ){
    this._testName = testName;
    this._connections = [];
    
    for ( var i=0; i<3; i++ ){
        this._connections.push( startMongodTest( 30000 + i , testName + i , false, extraMongodOptions ) );
    }
    
    this.url = this._connections.map( function(z){ return z.name; } ).join( "," );
    this.conn = new Mongo( this.url );
}

SyncCCTest.prototype.stop = function(){
    for ( var i=0; i<this._connections.length; i++){
        stopMongod( 30000 + i );
    }

    print('*** ' + this._testName + " completed successfully ***");
}

SyncCCTest.prototype.checkHashes = function( dbname , msg ){
    var hashes = this._connections.map(
        function(z){
            return z.getDB( dbname ).runCommand( "dbhash" );
        }
    );

    for ( var i=1; i<hashes.length; i++ ){
        assert.eq( hashes[0].md5 , hashes[i].md5 , "checkHash on " + dbname + " " + msg + "\n" + tojson( hashes ) )
    }
}

SyncCCTest.prototype.tempKill = function( num ){
    num = num || 0;
    stopMongod( 30000 + num );
}

SyncCCTest.prototype.tempStart = function( num ){
    num = num || 0;
    this._connections[num] = startMongodTest( 30000 + num , this._testName + num , true );
}


function startParallelShell( jsCode, port ){
    assert( jsCode.indexOf( '"' ) == -1,
           "double quotes should not be used in jsCode because the windows shell will stip them out" );
    var x;
    if ( port ) {
        x = startMongoProgramNoConnect( "mongo" , "--port" , port , "--eval" , jsCode );
    } else {
        x = startMongoProgramNoConnect( "mongo" , "--eval" , jsCode , db ? db.getMongo().host : null );        
    }
    return function(){
        waitProgram( x );
    };
}

var testingReplication = false;

function skipIfTestingReplication(){
    if (testingReplication) {
        print("skipIfTestingReplication skipping");
        quit(0);
    }
}

ReplSetTest = function( opts ){
    this.name  = opts.name || "testReplSet";
    this.host  = opts.host || getHostName();
    this.numNodes = opts.nodes || 0;
    this.oplogSize = opts.oplogSize || 40;
    this.useSeedList = opts.useSeedList || false;
    this.bridged = opts.bridged || false;
    this.ports = [];
    this.keyFile = opts.keyFile

    this.startPort = opts.startPort || 31000;

    if(this.bridged) {
        this.bridgePorts = [];

        var allPorts = allocatePorts( this.numNodes * 2 , this.startPort );
        for(var i=0; i < this.numNodes; i++) {
            this.ports[i] = allPorts[i*2];
            this.bridgePorts[i] = allPorts[i*2 + 1];
        }

        this.initBridges();
    }
    else {
        this.ports = allocatePorts( this.numNodes , this.startPort );
    }

    this.nodes = [];
    this.nodeIds = {};
    this.initLiveNodes();
    
    Object.extend( this, ReplSetTest.Health )
    Object.extend( this, ReplSetTest.State )
    
}

ReplSetTest.prototype.initBridges = function() {
    for(var i=0; i<this.ports.length; i++) {
        startMongoProgram( "mongobridge", "--port", this.bridgePorts[i], "--dest", this.host + ":" + this.ports[i] );
    }
}

// List of nodes as host:port strings.
ReplSetTest.prototype.nodeList = function() {
    var list = [];
    for(var i=0; i<this.ports.length; i++) {
      list.push( this.host + ":" + this.ports[i]);
    }

    return list;
}

// Here we store a reference to all reachable nodes.
ReplSetTest.prototype.initLiveNodes = function(){
    this.liveNodes = {master: null, slaves: []};
    this.nodeIds   = {};
}

ReplSetTest.prototype.getNodeId = function(node) {
    
    var result = this.nodeIds[node]
    if( result ) return result
    
    if( node.toFixed ) return node
    return node.nodeId
    
}

ReplSetTest.prototype.getPort = function( n ){
    if( n.getDB ){
        // is a connection, look up
        for( var i = 0; i < this.nodes.length; i++ ){
            if( this.nodes[i] == n ){
                n = i
                break
            }
        }
    }
    
    if ( typeof(n) == "object" && n.floatApprox )
        n = n.floatApprox
    
    // this is a hack for NumberInt
    if ( n == 0 )
        n = 0;
    
    print( "ReplSetTest n: " + n + " ports: " + tojson( this.ports ) + "\t" + this.ports[n] + " " + typeof(n) );
    return this.ports[ n ];
}

ReplSetTest.prototype.getPath = function( n ){
    
    if( n.host )
        n = this.getNodeId( n )

    var p = "/data/db/" + this.name + "-"+n;
    if ( ! this._alldbpaths )
        this._alldbpaths = [ p ];
    else
        this._alldbpaths.push( p );
    return p;
}

ReplSetTest.prototype.getReplSetConfig = function() {
    var cfg = {};

    cfg['_id']  = this.name;
    cfg.members = [];

    for(i=0; i<this.ports.length; i++) {
        member = {};
        member['_id']  = i;

        if(this.bridged)
          var port = this.bridgePorts[i];
        else
          var port = this.ports[i];

        member['host'] = this.host + ":" + port;
        cfg.members.push(member);
    }

    return cfg;
}

ReplSetTest.prototype.getURL = function(){
    var hosts = [];
    
    for(i=0; i<this.ports.length; i++) {

        // Don't include this node in the replica set list
        if(this.bridged && this.ports[i] == this.ports[n]) {
            continue;
        }
        
        var port;
        // Connect on the right port
        if(this.bridged) {
            port = this.bridgePorts[i];
        }
        else {
            port = this.ports[i];
        }
        
        var str = this.host + ":" + port;
        hosts.push(str);
    }
    
    return this.name + "/" + hosts.join(",");
}

ReplSetTest.prototype.getOptions = function( n , extra , putBinaryFirst ){

    if ( ! extra )
        extra = {};

    if ( ! extra.oplogSize )
        extra.oplogSize = this.oplogSize;

    var a = []


    if ( putBinaryFirst )
        a.push( "mongod" );

    if ( extra.noReplSet ) {
        delete extra.noReplSet;
    }
    else {
        a.push( "--replSet" );
        
        if( this.useSeedList ) {
            a.push( this.getURL() );
        }
        else {
            a.push( this.name );
        }
    }
    
    a.push( "--noprealloc", "--smallfiles" );

    a.push( "--rest" );

    a.push( "--port" );
    a.push( this.getPort( n ) );

    a.push( "--dbpath" );
    a.push( this.getPath( ( n.host ? this.getNodeId( n ) : n ) ) );
    
    if( this.keyFile ){
        a.push( "--keyFile" )
        a.push( keyFile )
    }        
    
    if( jsTestOptions().noJournal ) a.push( "--nojournal" )
    if( jsTestOptions().noJournalPrealloc ) a.push( "--nopreallocj" )
    
    for ( var k in extra ){
        var v = extra[k];        
        a.push( "--" + k );
        if ( v != null ){
            if( v.replace ){
                v = v.replace(/\$node/g, "" + ( n.host ? this.getNodeId( n ) : n ) )
                v = v.replace(/\$set/g, this.name )
                v = v.replace(/\$path/g, this.getPath( n ) )
            }
            a.push( v );
        }
    }

    return a;
}

ReplSetTest.prototype.startSet = function(options) {
    var nodes = [];
    print( "ReplSetTest Starting Set" );

    for(n=0; n<this.ports.length; n++) {
        node = this.start(n, options);
        nodes.push(node);
    }

    this.nodes = nodes;
    return this.nodes;
}

ReplSetTest.prototype.callIsMaster = function() {
  
  var master = null;
  this.initLiveNodes();
    
  for(var i=0; i<this.nodes.length; i++) {

    try {
      var n = this.nodes[i].getDB('admin').runCommand({ismaster:1});
      
      if(n['ismaster'] == true) {
        master = this.nodes[i];
        this.liveNodes.master = master;
        this.nodeIds[master] = i;
        master.nodeId = i
      }
      else {
        this.nodes[i].setSlaveOk();
        this.liveNodes.slaves.push(this.nodes[i]);
        this.nodeIds[this.nodes[i]] = i;
        this.nodes[i].nodeId = i
      }

    }
    catch(err) {
      print("ReplSetTest Could not call ismaster on node " + i);
    }
  }

  return master || false;
}

ReplSetTest.awaitRSClientHosts = function( conn, host, hostOk, rs ) {
    
    if( host.length ){
        for( var i = 0; i < host.length; i++ ) this.awaitOk( conn, host[i] )
        return
    }
    
    if( hostOk == undefined ) hostOk = { ok : true }
    if( host.host ) host = host.host
    if( rs && rs.getMaster ) rs = rs.name
    
    print( "Awaiting " + host + " to be " + tojson( hostOk ) + " for " + conn + " (rs: " + rs + ")" )
    
    var tests = 0
    assert.soon( function() {
        var rsClientHosts = conn.getDB( "admin" ).runCommand( "connPoolStats" )[ "replicaSets" ]
        if( tests++ % 10 == 0 ) 
            printjson( rsClientHosts )
        
        for ( rsName in rsClientHosts ){
            if( rs && rs != rsName ) continue
            for ( var i = 0; i < rsClientHosts[rsName].hosts.length; i++ ){
                var clientHost = rsClientHosts[rsName].hosts[ i ];
                if( clientHost.addr != host ) continue
                
                // Check that *all* host properties are set correctly
                var propOk = true
                for( var prop in hostOk ){
                    if( clientHost[prop] != hostOk[prop] ){ 
                        propOk = false
                        break
                    }
                }
                
                if( propOk ) return true;

            }
        }
        return false;
    }, "timed out waiting for replica set client to recognize hosts",
       3 * 20 * 1000 /* ReplicaSetMonitorWatcher updates every 20s */ )
    
}

ReplSetTest.prototype.awaitSecondaryNodes = function( timeout ) {
  var master = this.getMaster();
  var slaves = this.liveNodes.slaves;
  var len = slaves.length;

  this.attempt({context: this, timeout: 60000, desc: "Awaiting secondaries"}, function() {
     var ready = true;
     for(var i=0; i<len; i++) {
       ready = ready && slaves[i].getDB("admin").runCommand({ismaster: 1})['secondary'];
     }

     return ready;
  });
}

ReplSetTest.prototype.getMaster = function( timeout ) {
  var tries = 0;
  var sleepTime = 500;
  var t = timeout || 000;
  var master = null;

  master = this.attempt({context: this, timeout: 60000, desc: "Finding master"}, this.callIsMaster);
  return master;
}

ReplSetTest.prototype.getPrimary = ReplSetTest.prototype.getMaster

ReplSetTest.prototype.getSecondaries = function( timeout ){
    var master = this.getMaster( timeout )
    var secs = []
    for( var i = 0; i < this.nodes.length; i++ ){
        if( this.nodes[i] != master ){
            secs.push( this.nodes[i] )
        }
    }
    return secs
}

ReplSetTest.prototype.getSecondary = function( timeout ){
    return this.getSecondaries( timeout )[0];
}

ReplSetTest.prototype.status = function( timeout ){
    var master = this.callIsMaster()
    if( ! master ) master = this.liveNodes.slaves[0]
    return master.getDB("admin").runCommand({replSetGetStatus: 1})
}

// Add a node to the test set
ReplSetTest.prototype.add = function( config ) {
  if(this.ports.length == 0) {
    var nextPort = allocatePorts( 1, this.startPort )[0];
  }
  else {
    var nextPort = this.ports[this.ports.length-1] + 1;
  }
  print("ReplSetTest Next port: " + nextPort);
  this.ports.push(nextPort);
  printjson(this.ports);

  var nextId = this.nodes.length;
  printjson(this.nodes);
  print("ReplSetTest nextId:" + nextId);
  var newNode = this.start(nextId);
  this.nodes.push(newNode);

  return newNode;
}

ReplSetTest.prototype.remove = function( nodeId ) {
    this.nodes.splice( nodeId, 1 );
    this.ports.splice( nodeId, 1 );
}

// Pass this method a function to call repeatedly until
// that function returns true. Example:
//   attempt({timeout: 20000, desc: "get master"}, function() { // return false until success })
ReplSetTest.prototype.attempt = function( opts, func ) {
    var timeout = opts.timeout || 1000;
    var tries   = 0;
    var sleepTime = 500;
    var result = null;
    var context = opts.context || this;

    while((result = func.apply(context)) == false) {
        tries += 1;
        sleep(sleepTime);
        if( tries * sleepTime > timeout) {
            throw('[' + opts['desc'] + ']' + " timed out after " + timeout + "ms ( " + tries + " tries )");
        }
    }

    return result;
}

ReplSetTest.prototype.initiate = function( cfg , initCmd , timeout ) {
    var master  = this.nodes[0].getDB("admin");
    var config  = cfg || this.getReplSetConfig();
    var cmd     = {};
    var cmdKey  = initCmd || 'replSetInitiate';
    var timeout = timeout || 30000;
    cmd[cmdKey] = config;
    printjson(cmd);

    this.attempt({timeout: timeout, desc: "Initiate replica set"}, function() {
        var result = master.runCommand(cmd);
        printjson(result);
        return result['ok'] == 1;
    });
}

ReplSetTest.prototype.reInitiate = function() {
    var master  = this.nodes[0];
    var c = master.getDB("local")['system.replset'].findOne();
    var config  = this.getReplSetConfig();
    config.version = c.version + 1;
    this.initiate( config , 'replSetReconfig' );
}

ReplSetTest.prototype.getLastOpTimeWritten = function() {
    this.getMaster();
    this.attempt({context : this, desc : "awaiting oplog query"},
                 function() {
                     try {
                         this.latest = this.liveNodes.master.getDB("local")['oplog.rs'].find({}).sort({'$natural': -1}).limit(1).next()['ts'];
                     }
                     catch(e) {
                         print("ReplSetTest caught exception " + e);
                         return false;
                     }
                     return true;
                 });
};

ReplSetTest.prototype.awaitReplication = function(timeout) {
    timeout = timeout || 30000;

    this.getLastOpTimeWritten();

    print("ReplSetTest " + this.latest);

    this.attempt({context: this, timeout: timeout, desc: "awaiting replication"},
                 function() {
                     try {
                         var synced = true;
                         for(var i=0; i<this.liveNodes.slaves.length; i++) {
                             var slave = this.liveNodes.slaves[i];

                             // Continue if we're connected to an arbiter
                             if(res = slave.getDB("admin").runCommand({replSetGetStatus: 1})) {
                                 if(res.myState == 7) {
                                     continue;
                                 }
                             }

                             slave.getDB("admin").getMongo().setSlaveOk();
                             var log = slave.getDB("local")['oplog.rs'];
                             if(log.find({}).sort({'$natural': -1}).limit(1).hasNext()) {
                                 var entry = log.find({}).sort({'$natural': -1}).limit(1).next();
                                 printjson( entry );
                                 var ts = entry['ts'];
                                 print("ReplSetTest await TS for " + slave + " is " + ts.t+":"+ts.i + " and latest is " + this.latest.t+":"+this.latest.i);

                                 if (this.latest.t < ts.t || (this.latest.t == ts.t && this.latest.i < ts.i)) {
                                     this.latest = this.liveNodes.master.getDB("local")['oplog.rs'].find({}).sort({'$natural': -1}).limit(1).next()['ts'];
                                 }

                                 print("ReplSetTest await oplog size for " + slave + " is " + log.count());
                                 synced = (synced && friendlyEqual(this.latest,ts))
                             }
                             else {
                                 synced = false;
                             }
                         }

                         if(synced) {
                             print("ReplSetTest await synced=" + synced);
                         }
                         return synced;
                     }
                     catch (e) {
                         print("ReplSetTest.awaitReplication: caught exception "+e);

                         // we might have a new master now
                         this.getLastOpTimeWritten();

                         return false;
                     }
                 });
}

ReplSetTest.prototype.getHashes = function( db ){
    this.getMaster();
    var res = {};
    res.master = this.liveNodes.master.getDB( db ).runCommand( "dbhash" )
    res.slaves = this.liveNodes.slaves.map( function(z){ return z.getDB( db ).runCommand( "dbhash" ); } )
    return res;
}

/**
 * Starts up a server.  Options are saved by default for subsequent starts.
 * 
 * 
 * Options { remember : true } re-applies the saved options from a prior start.
 * Options { noRemember : true } ignores the current properties.
 * Options { appendOptions : true } appends the current options to those remembered.
 * Options { startClean : true } clears the data directory before starting.
 *
 * @param @param {int|conn|[int|conn]} n array or single server number (0, 1, 2, ...) or conn
 * @param {object} [options]
 * @param {boolean} [restart] If false, the data directory will be cleared 
 * before the server starts.  Defaults to false.
 * 
 */
ReplSetTest.prototype.start = function( n , options , restart , wait ){
    
    if( n.length ){
        
        var nodes = n
        var started = []
        
        for( var i = 0; i < nodes.length; i++ ){
            if( this.start( nodes[i], Object.extend({}, options), restart, wait ) ){
                started.push( nodes[i] )
            }
        }
        
        return started
        
    }
    
    print( "ReplSetTest n is : " + n )
    
    var lockFile = this.getPath( n ) + "/mongod.lock";
    removeFile( lockFile );
    
    options = options || {}
    var noRemember = options.noRemember
    delete options.noRemember
    var appendOptions = options.appendOptions
    delete options.appendOptions
    var startClean = options.startClean
    delete options.startClean
    
    if( restart && options.remember ){
        delete options.remember
        
        var oldOptions = {}
        if( this.savedStartOptions && this.savedStartOptions[n] ){
            oldOptions = this.savedStartOptions[n]
        }        
        
        var newOptions = options
        var options = {}
        Object.extend( options, oldOptions )
        Object.extend( options, newOptions )
        
    }
    
    var shouldRemember = ( ! restart && ! noRemember ) || ( restart && appendOptions ) 
    
    if ( shouldRemember ){
        this.savedStartOptions = this.savedStartOptions || {}
        this.savedStartOptions[n] = options
    }
    
    if( tojson(options) != tojson({}) )
        printjson(options)
                
    var o = this.getOptions( n , options , restart && ! startClean );

    print("ReplSetTest " + (restart ? "(Re)" : "") + "Starting....");
    print("ReplSetTest " + o );
    
    var rval = null
    if ( restart ) {
        n = this.getNodeId( n )
        this.nodes[n] = ( startClean ? startMongod.apply( null , o ) : startMongoProgram.apply( null , o ) );
        this.nodes[n].host = this.nodes[n].host.replace( "127.0.0.1", this.host )
        if( shouldRemember ) this.savedStartOptions[this.nodes[n]] = options
		printjson( this.nodes )
        rval = this.nodes[n];
    }
    else {
       var conn = startMongod.apply( null , o );
       if( shouldRemember ) this.savedStartOptions[conn] = options
       conn.host = conn.host.replace( "127.0.0.1", this.host )
       rval = conn;
    }
    
    wait = wait || false
    if( ! wait.toFixed ){
        if( wait ) wait = 0
        else wait = -1
    }
    
    if( rval == null || wait < 0 ) return rval
    
    // Wait for startup
    this.waitForHealth( rval, this.UP, wait )
    
    return rval
    
}


/**
 * Restarts a db without clearing the data directory by default.  If the server is not
 * stopped first, this function will not work.  
 * 
 * Option { startClean : true } forces clearing the data directory.
 * 
 * @param {int|conn|[int|conn]} n array or single server number (0, 1, 2, ...) or conn
 */
ReplSetTest.prototype.restart = function( n , options, signal, wait ){
    // Can specify wait as third parameter, if using default signal
    if( signal == true || signal == false ){
        wait = signal
        signal = undefined
    }
    
    this.stop( n, signal, wait && wait.toFixed ? wait : true )
    return this.start( n , options , true, wait );
}

ReplSetTest.prototype.stopMaster = function( signal , wait ) {
    var master = this.getMaster();
    var master_id = this.getNodeId( master );
    return this.stop( master_id , signal , wait );
}

// Stops a particular node or nodes, specified by conn or id
ReplSetTest.prototype.stop = function( n , signal, wait /* wait for stop */ ){
        
    // Flatten array of nodes to stop
    if( n.length ){
        nodes = n
        
        var stopped = []
        for( var i = 0; i < nodes.length; i++ ){
            if( this.stop( nodes[i], signal, wait ) )
                stopped.push( nodes[i] )
        }
        
        return stopped
    }
    

    // Can specify wait as second parameter, if using default signal
    if( signal == true || signal == false ){
        wait = signal
        signal = undefined
    }
        
    wait = wait || false
    if( ! wait.toFixed ){
        if( wait ) wait = 0
        else wait = -1
    }
    
    var port = this.getPort( n );
    print('ReplSetTest stop *** Shutting down mongod in port ' + port + ' ***');
    var ret = stopMongod( port , signal || 15 );
    
    if( ! ret || wait < 0 ) return ret
    
    // Wait for shutdown
    this.waitForHealth( n, this.DOWN, wait )
    
    return true
}


ReplSetTest.prototype.stopSet = function( signal , forRestart ) {
    for(i=0; i < this.ports.length; i++) {
        this.stop( i, signal );
    }
    if ( ! forRestart && this._alldbpaths ){
        print("ReplSetTest stopSet deleting all dbpaths");
        for( i=0; i<this._alldbpaths.length; i++ ){
            resetDbpath( this._alldbpaths[i] );
        }
    }

    print('ReplSetTest stopSet *** Shut down repl set - test worked ****' )
};


/**
 * Waits until there is a master node
 */
ReplSetTest.prototype.waitForMaster = function( timeout ){
    
    var master = undefined
    
    this.attempt({context: this, timeout: timeout, desc: "waiting for master"}, function() {
        return ( master = this.getMaster() )
    });
    
    return master
}


/**
 * Wait for a health indicator to go to a particular state or states.
 * 
 * @param node is a single node or list of nodes, by id or conn
 * @param state is a single state or list of states
 * 
 */
ReplSetTest.prototype.waitForHealth = function( node, state, timeout ){
    this.waitForIndicator( node, state, "health", timeout )    
}

/**
 * Wait for a state indicator to go to a particular state or states.
 * 
 * @param node is a single node or list of nodes, by id or conn
 * @param state is a single state or list of states
 * 
 */
ReplSetTest.prototype.waitForState = function( node, state, timeout ){
    this.waitForIndicator( node, state, "state", timeout )
}

/**
 * Wait for a rs indicator to go to a particular state or states.
 * 
 * @param node is a single node or list of nodes, by id or conn
 * @param states is a single state or list of states
 * @param ind is the indicator specified
 * 
 */
ReplSetTest.prototype.waitForIndicator = function( node, states, ind, timeout ){
    
    if( node.length ){
        
        var nodes = node        
        for( var i = 0; i < nodes.length; i++ ){
            if( states.length )
                this.waitForIndicator( nodes[i], states[i], ind, timeout )
            else
                this.waitForIndicator( nodes[i], states, ind, timeout )
        }
        
        return;
    }    
    
    timeout = timeout || 30000;
    
    if( ! node.getDB ){
        node = this.nodes[node]
    }
    
    if( ! states.length ) states = [ states ]
    
    print( "ReplSetTest waitForIndicator " + ind )
    printjson( states )
    print( "ReplSetTest waitForIndicator from node " + node )
    
    var lastTime = null
    var currTime = new Date().getTime()
    var status = undefined
        
    this.attempt({context: this, timeout: timeout, desc: "waiting for state indicator " + ind + " for " + timeout + "ms" }, function() {
        
        status = this.status()
        
        if( lastTime == null || ( currTime = new Date().getTime() ) - (1000 * 5) > lastTime ){
            if( lastTime == null ) print( "ReplSetTest waitForIndicator Initial status ( timeout : " + timeout + " ) :" )
            printjson( status )
            lastTime = new Date().getTime()
        }

        if (typeof status.members == 'undefined') {
            return false;
        }

        for( var i = 0; i < status.members.length; i++ ){
            if( status.members[i].name == node.host ){
                for( var j = 0; j < states.length; j++ ){
                    if( status.members[i][ind] == states[j] ) return true;
                }
            }
        }
        
        return false
        
    });
    
    print( "ReplSetTest waitForIndicator final status:" )
    printjson( status )
    
}

ReplSetTest.Health = {}
ReplSetTest.Health.UP = 1
ReplSetTest.Health.DOWN = 0

ReplSetTest.State = {}
ReplSetTest.State.PRIMARY = 1
ReplSetTest.State.SECONDARY = 2
ReplSetTest.State.RECOVERING = 3

/** 
 * Overflows a replica set secondary or secondaries, specified by id or conn.
 */
ReplSetTest.prototype.overflow = function( secondaries ){
    
    // Create a new collection to overflow, allow secondaries to replicate
    var master = this.getMaster()
    var overflowColl = master.getCollection( "_overflow.coll" )
    overflowColl.insert({ replicated : "value" })
    this.awaitReplication()
    
    this.stop( secondaries, undefined, 5 * 60 * 1000 )
        
    var count = master.getDB("local").oplog.rs.count();
    var prevCount = -1;
    
    // Keep inserting till we hit our capped coll limits
    while (count != prevCount) {
      
      print("ReplSetTest overflow inserting 10000");
      
      for (var i = 0; i < 10000; i++) {
          overflowColl.insert({ overflow : "value" });
      }
      prevCount = count;
      this.awaitReplication();
      
      count = master.getDB("local").oplog.rs.count();
      
      print( "ReplSetTest overflow count : " + count + " prev : " + prevCount );
      
    }
    
    // Restart all our secondaries and wait for recovery state
    this.start( secondaries, { remember : true }, true, true )
    this.waitForState( secondaries, this.RECOVERING, 5 * 60 * 1000 )
    
}




/**
 * Bridging allows you to test network partitioning.  For example, you can set
 * up a replica set, run bridge(), then kill the connection between any two
 * nodes x and y with partition(x, y).
 *
 * Once you have called bridging, you cannot reconfigure the replica set.
 */
ReplSetTest.prototype.bridge = function() {
    if (this.bridges) {
        print("ReplSetTest bridge bridges have already been created!");
        return;
    }
    
    var n = this.nodes.length;

    // create bridges
    this.bridges = [];
    for (var i=0; i<n; i++) {
        var nodeBridges = [];
        for (var j=0; j<n; j++) {
            if (i == j) {
                continue;
            }
            nodeBridges[j] = new ReplSetBridge(this, i, j);
        }
        this.bridges.push(nodeBridges);
    }
    print("ReplSetTest bridge bridges: " + this.bridges);
    
    // restart everyone independently
    this.stopSet(null, true);
    for (var i=0; i<n; i++) {
        this.restart(i, {noReplSet : true});
    }
    
    // create new configs
    for (var i=0; i<n; i++) {
        config = this.nodes[i].getDB("local").system.replset.findOne();
        
        if (!config) {
            print("ReplSetTest bridge couldn't find config for "+this.nodes[i]);
            printjson(this.nodes[i].getDB("local").system.namespaces.find().toArray());
            assert(false);
        }

        var updateMod = {"$set" : {}};
        for (var j = 0; j<config.members.length; j++) {
            if (config.members[j].host == this.host+":"+this.ports[i]) {
                continue;
            }

            updateMod['$set']["members."+j+".host"] = this.bridges[i][j].host;
        }
        print("ReplSetTest bridge for node " + i + ":");
        printjson(updateMod);
        this.nodes[i].getDB("local").system.replset.update({},updateMod);
    }

    this.stopSet(null, true);
    
    // start set
    for (var i=0; i<n; i++) {
        this.restart(i);
    }

    return this.getMaster();
};

/**
 * This kills the bridge between two nodes.  As parameters, specify the from and
 * to node numbers.
 *
 * For example, with a three-member replica set, we'd have nodes 0, 1, and 2,
 * with the following bridges: 0->1, 0->2, 1->0, 1->2, 2->0, 2->1.  We can kill
 * the connection between nodes 0 and 2 by calling replTest.partition(0,2) or
 * replTest.partition(2,0) (either way is identical). Then the replica set would
 * have the following bridges: 0->1, 1->0, 1->2, 2->1.
 */
ReplSetTest.prototype.partition = function(from, to) {
    this.bridges[from][to].stop();
    this.bridges[to][from].stop();
};

/**
 * This reverses a partition created by partition() above.
 */
ReplSetTest.prototype.unPartition = function(from, to) {
    this.bridges[from][to].start();
    this.bridges[to][from].start();
};

ReplSetBridge = function(rst, from, to) {
    var n = rst.nodes.length;

    var startPort = rst.startPort+n;
    this.port = (startPort+(from*n+to));
    this.host = rst.host+":"+this.port;

    this.dest = rst.host+":"+rst.ports[to];
    this.start();
};

ReplSetBridge.prototype.start = function() {
    var args = ["mongobridge", "--port", this.port, "--dest", this.dest];
    print("ReplSetBridge starting: "+tojson(args));
    this.bridge = startMongoProgram.apply( null , args );
    print("ReplSetBridge started " + this.bridge);
};

ReplSetBridge.prototype.stop = function() {
    print("ReplSetBridge stopping: " + this.port);
    stopMongod(this.port);
};

ReplSetBridge.prototype.toString = function() {
    return this.host+" -> "+this.dest;
};
