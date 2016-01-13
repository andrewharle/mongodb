// db.js

var DB;

(function() {

if (DB === undefined) {
    DB = function( mongo , name ){
        this._mongo = mongo;
        this._name = name;
    }
}

DB.prototype.getMongo = function(){
    assert( this._mongo , "why no mongo!" );
    return this._mongo;
}

DB.prototype.getSiblingDB = function( name ){
    return this.getMongo().getDB( name );
}

DB.prototype.getSisterDB = DB.prototype.getSiblingDB;

DB.prototype.getName = function(){
    return this._name;
}

DB.prototype.stats = function(scale){
    return this.runCommand( { dbstats : 1 , scale : scale } );
}

DB.prototype.getCollection = function( name ){
    return new DBCollection( this._mongo , this , name , this._name + "." + name );
}

DB.prototype.commandHelp = function( name ){
    var c = {};
    c[name] = 1;
    c.help = true;
    var res = this.runCommand( c );
    if ( ! res.ok )
        throw res.errmsg;
    return res.help;
}

DB.prototype.runCommand = function( obj ){
    if ( typeof( obj ) == "string" ){
        var n = {};
        n[obj] = 1;
        obj = n;
    }
    return this.getCollection( "$cmd" ).findOne( obj );
}

DB.prototype._dbCommand = DB.prototype.runCommand;

DB.prototype.adminCommand = function( obj ){
    if ( this._name == "admin" )
        return this.runCommand( obj );
    return this.getSiblingDB( "admin" ).runCommand( obj );
}

DB.prototype._adminCommand = DB.prototype.adminCommand; // alias old name

/**
  Create a new collection in the database.  Normally, collection creation is automatic.  You would
   use this function if you wish to specify special options on creation.

   If the collection already exists, no action occurs.

    <p>Options:</p>
    <ul>
    <li>
        size: desired initial extent size for the collection.  Must be <= 1000000000.
              for fixed size (capped) collections, this size is the total/max size of the 
              collection.
    </li>
    <li>
        capped: if true, this is a capped collection (where old data rolls out).
    </li>
    <li> max: maximum number of objects if capped (optional).</li>
    <li> usePowerOf2Sizes: if true, set usePowerOf2Sizes allocation for the collection.</li>
    </ul>

    <p>Example:</p>
    <code>db.createCollection("movies", { size: 10 * 1024 * 1024, capped:true } );</code>

 * @param {String} name Name of new collection to create 
 * @param {Object} options Object with options for call.  Options are listed above.
 * @return SOMETHING_FIXME
*/
DB.prototype.createCollection = function(name, opt) {
    var options = opt || {};
    var cmd = { create: name };
    if (options.max != undefined)
        cmd.max = options.max;
    if (options.autoIndexId != undefined)
        cmd.autoIndexId = options.autoIndexId;
    if (options.capped != undefined)
        cmd.capped = options.capped;
    if (options.size != undefined)
        cmd.size = options.size;
    if (options.usePowerOf2Sizes != undefined) 
        cmd.flags = options.usePowerOf2Sizes ? 1 : 0;
    var res = this._dbCommand(cmd);
    return res;
}

/**
 * @deprecated use getProfilingStatus
 *  Returns the current profiling level of this database
 *  @return SOMETHING_FIXME or null on error
 */
DB.prototype.getProfilingLevel  = function() {
    var res = this._dbCommand( { profile: -1 } );
    return res ? res.was : null;
}

/**
 *  @return the current profiling status
 *  example { was : 0, slowms : 100 }
 *  @return SOMETHING_FIXME or null on error
 */
DB.prototype.getProfilingStatus  = function() {
    var res = this._dbCommand( { profile: -1 } );
    if ( ! res.ok )
        throw "profile command failed: " + tojson( res );
    delete res.ok
    return res;
}


/**
  Erase the entire database.  (!)

 * @return Object returned has member ok set to true if operation succeeds, false otherwise.
 */
DB.prototype.dropDatabase = function() {
    if ( arguments.length )
        throw "dropDatabase doesn't take arguments";
    return this._dbCommand( { dropDatabase: 1 } );
}

/**
 * Shuts down the database.  Must be run while using the admin database.
 * @param opts Options for shutdown. Possible options are:
 *   - force: (boolean) if the server should shut down, even if there is no
 *     up-to-date slave
 *   - timeoutSecs: (number) the server will continue checking over timeoutSecs
 *     if any other servers have caught up enough for it to shut down.
 */
DB.prototype.shutdownServer = function(opts) {
    if( "admin" != this._name ){
        return "shutdown command only works with the admin database; try 'use admin'";
    }

    cmd = {"shutdown" : 1};
    opts = opts || {};
    for (var o in opts) {
        cmd[o] = opts[o];
    }

    try {
        var res = this.runCommand(cmd);
        if( res )
            throw "shutdownServer failed: " + res.errmsg;
        throw "shutdownServer failed";
    }
    catch ( e ){
        assert( tojson( e ).indexOf( "error doing query: failed" ) >= 0 , "unexpected error: " + tojson( e ) );
        print( "server should be down..." );
    }
}

/**
  Clone database on another server to here.
  <p>
  Generally, you should dropDatabase() first as otherwise the cloned information will MERGE 
  into whatever data is already present in this database.  (That is however a valid way to use 
  clone if you are trying to do something intentionally, such as union three non-overlapping
  databases into one.)
  <p>
  This is a low level administrative function will is not typically used.

 * @param {String} from Where to clone from (dbhostname[:port]).  May not be this database 
                   (self) as you cannot clone to yourself.
 * @return Object returned has member ok set to true if operation succeeds, false otherwise.
 * See also: db.copyDatabase()
 */
DB.prototype.cloneDatabase = function(from) { 
    assert( isString(from) && from.length );
    return this._dbCommand( { clone: from } );
}


/**
 Clone collection on another server to here.
 <p>
 Generally, you should drop() first as otherwise the cloned information will MERGE 
 into whatever data is already present in this collection.  (That is however a valid way to use 
 clone if you are trying to do something intentionally, such as union three non-overlapping
 collections into one.)
 <p>
 This is a low level administrative function is not typically used.
 
 * @param {String} from mongod instance from which to clnoe (dbhostname:port).  May
 not be this mongod instance, as clone from self is not allowed.
 * @param {String} collection name of collection to clone.
 * @param {Object} query query specifying which elements of collection are to be cloned.
 * @return Object returned has member ok set to true if operation succeeds, false otherwise.
 * See also: db.cloneDatabase()
 */
DB.prototype.cloneCollection = function(from, collection, query) { 
    assert( isString(from) && from.length );
    assert( isString(collection) && collection.length );
    collection = this._name + "." + collection;
    query = query || {};
    return this._dbCommand( { cloneCollection:collection, from:from, query:query } );
}


/**
  Copy database from one server or name to another server or name.

  Generally, you should dropDatabase() first as otherwise the copied information will MERGE 
  into whatever data is already present in this database (and you will get duplicate objects 
  in collections potentially.)

  For security reasons this function only works when executed on the "admin" db.  However, 
  if you have access to said db, you can copy any database from one place to another.

  This method provides a way to "rename" a database by copying it to a new db name and 
  location.  Additionally, it effectively provides a repair facility.

  * @param {String} fromdb database name from which to copy.
  * @param {String} todb database name to copy to.
  * @param {String} fromhost hostname of the database (and optionally, ":port") from which to 
                    copy the data.  default if unspecified is to copy from self.
  * @return Object returned has member ok set to true if operation succeeds, false otherwise.
  * See also: db.clone()
*/
DB.prototype.copyDatabase = function(fromdb, todb, fromhost, username, password) { 
    assert( isString(fromdb) && fromdb.length );
    assert( isString(todb) && todb.length );
    fromhost = fromhost || "";
    if ( username && password ) {
        var n = this._adminCommand( { copydbgetnonce : 1, fromhost:fromhost } );
        return this._adminCommand( { copydb:1, fromhost:fromhost, fromdb:fromdb, todb:todb, username:username, nonce:n.nonce, key:this.__pwHash( n.nonce, username, password ) } );
    } else {
        return this._adminCommand( { copydb:1, fromhost:fromhost, fromdb:fromdb, todb:todb } );
    }
}

/**
  Repair database.
 
 * @return Object returned has member ok set to true if operation succeeds, false otherwise.
*/
DB.prototype.repairDatabase = function() {
    return this._dbCommand( { repairDatabase: 1 } );
}


DB.prototype.help = function() {
    print("DB methods:");
    print("\tdb.adminCommand(nameOrDocument) - switches to 'admin' db, and runs command [ just calls db.runCommand(...) ]");
    print("\tdb.auth(username, password)");
    print("\tdb.cloneDatabase(fromhost)");
    print("\tdb.commandHelp(name) returns the help for the command");
    print("\tdb.copyDatabase(fromdb, todb, fromhost)");
    print("\tdb.createCollection(name, { size : ..., capped : ..., max : ... } )");
    print("\tdb.createUser(userDocument)");
    print("\tdb.currentOp() displays currently executing operations in the db");
    print("\tdb.dropDatabase()");
    print("\tdb.eval(func, args) run code server-side");
    print("\tdb.fsyncLock() flush data to disk and lock server for backups");
    print("\tdb.fsyncUnlock() unlocks server following a db.fsyncLock()");
    print("\tdb.getCollection(cname) same as db['cname'] or db.cname");
    print("\tdb.getCollectionInfos()");
    print("\tdb.getCollectionNames()");
    print("\tdb.getLastError() - just returns the err msg string");
    print("\tdb.getLastErrorObj() - return full status object");
    print("\tdb.getMongo() get the server connection object");
    print("\tdb.getMongo().setSlaveOk() allow queries on a replication slave server");
    print("\tdb.getName()");
    print("\tdb.getPrevError()");
    print("\tdb.getProfilingLevel() - deprecated");
    print("\tdb.getProfilingStatus() - returns if profiling is on and slow threshold");
    print("\tdb.getReplicationInfo()");
    print("\tdb.getSiblingDB(name) get the db at the same server as this one");
    print("\tdb.getWriteConcern() - returns the write concern used for any operations on this db, inherited from server object if set");
    print("\tdb.hostInfo() get details about the server's host"); 
    print("\tdb.isMaster() check replica primary status");
    print("\tdb.killOp(opid) kills the current operation in the db");
    print("\tdb.listCommands() lists all the db commands");
    print("\tdb.loadServerScripts() loads all the scripts in db.system.js");
    print("\tdb.logout()");
    print("\tdb.printCollectionStats()");
    print("\tdb.printReplicationInfo()");
    print("\tdb.printShardingStatus()");
    print("\tdb.printSlaveReplicationInfo()");
    print("\tdb.dropUser(username)");
    print("\tdb.repairDatabase()");
    print("\tdb.resetError()");
    print("\tdb.runCommand(cmdObj) run a database command.  if cmdObj is a string, turns it into { cmdObj : 1 }");
    print("\tdb.serverStatus()");
    print("\tdb.setProfilingLevel(level,<slowms>) 0=off 1=slow 2=all");
    print("\tdb.setWriteConcern( <write concern doc> ) - sets the write concern for writes to the db");
    print("\tdb.unsetWriteConcern( <write concern doc> ) - unsets the write concern for writes to the db");
    print("\tdb.setVerboseShell(flag) display extra information in shell output");
    print("\tdb.shutdownServer()");
    print("\tdb.stats()");
    print("\tdb.version() current version of the server");

    return __magicNoPrint;
}

DB.prototype.printCollectionStats = function(scale) { 
    if (arguments.length > 1) { 
        print("printCollectionStats() has a single optional argument (scale)");
        return;
    }
    if (typeof scale != 'undefined') {
        if(typeof scale != 'number') {
            print("scale has to be a number >= 1");
            return;
        }
        if (scale < 1) {
            print("scale has to be >= 1");
            return;
        }
    }
    var mydb = this;
    this.getCollectionNames().forEach(
        function(z) {
            print( z );
            printjson( mydb.getCollection(z).stats(scale) );
            print( "---" );
        }
    );
}

/**
 * <p> Set profiling level for your db.  Profiling gathers stats on query performance. </p>
 * 
 * <p>Default is off, and resets to off on a database restart -- so if you want it on,
 *    turn it on periodically. </p>
 *  
 *  <p>Levels :</p>
 *   <ul>
 *    <li>0=off</li>
 *    <li>1=log very slow operations; optional argument slowms specifies slowness threshold</li>
 *    <li>2=log all</li>
 *  @param {String} level Desired level of profiling
 *  @param {String} slowms For slow logging, query duration that counts as slow (default 100ms)
 *  @return SOMETHING_FIXME or null on error
 */
DB.prototype.setProfilingLevel = function(level,slowms) {
    
    if (level < 0 || level > 2) { 
        var errorText = "input level " + level + " is out of range [0..2]";
        var errorObject = new Error(errorText);
        errorObject['dbSetProfilingException'] = errorText;
        throw errorObject;
    }

    var cmd = { profile: level };
    if ( isNumber( slowms ) )
        cmd["slowms"] = slowms;
    return this._dbCommand( cmd );
}

/**
 *  <p> Evaluate a js expression at the database server.</p>
 * 
 * <p>Useful if you need to touch a lot of data lightly; in such a scenario
 *  the network transfer of the data could be a bottleneck.  A good example
 *  is "select count(*)" -- can be done server side via this mechanism.
 * </p>
 *
 * <p>
 * If the eval fails, an exception is thrown of the form:
 * </p>
 * <code>{ dbEvalException: { retval: functionReturnValue, ok: num [, errno: num] [, errmsg: str] } }</code>
 * 
 * <p>Example: </p>
 * <code>print( "mycount: " + db.eval( function(){db.mycoll.find({},{_id:ObjId()}).length();} );</code>
 *
 * @param {Function} jsfunction Javascript function to run on server.  Note this it not a closure, but rather just "code".
 * @return result of your function, or null if error
 * 
 */
DB.prototype.eval = function(jsfunction) {
    var cmd = { $eval : jsfunction };
    if ( arguments.length > 1 ) {
        cmd.args = argumentsToArray( arguments ).slice(1);
    }
    
    var res = this._dbCommand( cmd );
    
    if (!res.ok)
        throw tojson( res );
    
    return res.retval;
}

DB.prototype.dbEval = DB.prototype.eval;


/**
 * 
 *  <p>
 *   Similar to SQL group by.  For example: </p>
 *
 *  <code>select a,b,sum(c) csum from coll where active=1 group by a,b</code>
 *
 *  <p>
 *    corresponds to the following in 10gen:
 *  </p>
 * 
 *  <code>
    db.group(
        {
            ns: "coll",
            key: { a:true, b:true },
            // keyf: ...,
            cond: { active:1 },
            reduce: function(obj,prev) { prev.csum += obj.c; },
            initial: { csum: 0 }
        });
    </code>
 *
 * 
 * <p>
 *  An array of grouped items is returned.  The array must fit in RAM, thus this function is not
 * suitable when the return set is extremely large.
 * </p>
 * <p>
 * To order the grouped data, simply sort it client side upon return.
 * <p>
   Defaults
     cond may be null if you want to run against all rows in the collection
     keyf is a function which takes an object and returns the desired key.  set either key or keyf (not both).
 * </p>
 */
DB.prototype.groupeval = function(parmsObj) {

    var groupFunction = function() {
        var parms = args[0];
        var c = db[parms.ns].find(parms.cond||{});
        var map = new Map();
        var pks = parms.key ? Object.keySet( parms.key ) : null;
        var pkl = pks ? pks.length : 0;
        var key = {};

        while( c.hasNext() ) {
            var obj = c.next();
            if ( pks ) {
                for ( var i=0; i<pkl; i++ ) {
                    var k = pks[i];
                    key[k] = obj[k];
                }
            }
            else {
                key = parms.$keyf(obj);
            }

            var aggObj = map.get(key);
            if( aggObj == null ) {
                var newObj = Object.extend({}, key); // clone
                aggObj = Object.extend(newObj, parms.initial);
                map.put( key, aggObj );
            }
            parms.$reduce(obj, aggObj);
        }

        return map.values();
    }

    return this.eval(groupFunction, this._groupFixParms( parmsObj ));
}

DB.prototype.groupcmd = function( parmsObj ){
    var ret = this.runCommand( { "group" : this._groupFixParms( parmsObj ) } );
    if ( ! ret.ok ){
        throw "group command failed: " + tojson( ret );
    }
    return ret.retval;
}

DB.prototype.group = DB.prototype.groupcmd;

DB.prototype._groupFixParms = function( parmsObj ){
    var parms = Object.extend({}, parmsObj);

    if( parms.reduce ) {
        parms.$reduce = parms.reduce; // must have $ to pass to db
        delete parms.reduce;
    }

    if( parms.keyf ) {
        parms.$keyf = parms.keyf;
        delete parms.keyf;
    }

    return parms;
}

DB.prototype.resetError = function(){
    return this.runCommand( { reseterror : 1 } );
}

DB.prototype.forceError = function(){
    return this.runCommand( { forceerror : 1 } );
}

DB.prototype.getLastError = function( w , wtimeout ){
    var res = this.getLastErrorObj( w , wtimeout );
    if ( ! res.ok )
        throw "getlasterror failed: " + tojson( res );
    return res.err;
}
DB.prototype.getLastErrorObj = function( w , wtimeout ){
    var cmd = { getlasterror : 1 };
    if ( w ){
        cmd.w = w;
        if ( wtimeout )
            cmd.wtimeout = wtimeout;
    }
    var res = this.runCommand( cmd );

    if ( ! res.ok )
        throw "getlasterror failed: " + tojson( res );
    return res;
}
DB.prototype.getLastErrorCmd = DB.prototype.getLastErrorObj;


/* Return the last error which has occurred, even if not the very last error.

   Returns: 
    { err : <error message>, nPrev : <how_many_ops_back_occurred>, ok : 1 }

   result.err will be null if no error has occurred.
 */
DB.prototype.getPrevError = function(){
    return this.runCommand( { getpreverror : 1 } );
}

DB.prototype._getCollectionInfosSystemNamespaces = function(){
    var all = [];

    var nsLength = this._name.length + 1;
    
    var c = this.getCollection( "system.namespaces" ).find();
    while ( c.hasNext() ){
        var infoObj = c.next();
        
        if ( infoObj.name.indexOf( "$" ) >= 0 && infoObj.name.indexOf( ".oplog.$" ) < 0 )
            continue;
        
        infoObj.name = infoObj.name.substring( nsLength );

        all.push( infoObj );
    }
    
    // Return list of objects sorted by collection name.
    return all.sort(function(coll1, coll2) { return coll1.name.localeCompare(coll2.name); });
}


DB.prototype._getCollectionInfosCommand = function() {
    try {
        var res = this.runCommand( "listCollections" );
    }
    catch (e) {
        // command doesn't exist, very old mongod
        return null;
    }

    if ( res.code == 59 ) {
        // command doesn't exist, old mongod
        return null;
    }

    if ( !res.ok ) {
        if ( res.errmsg && res.errmsg.startsWith( "no such cmd" ) ) {
            return null;
        }

        throw Error( "listCollections failed: " + tojson( res ) );
    }

    // The listCollections command returns its results sorted by collection name.  There's no need
    // to re-sort.
    return new DBCommandCursor(this._mongo, res).toArray();
}

/**
 * Returns this database's list of collection metadata objects, sorted by collection name.
 */
DB.prototype.getCollectionInfos = function() {
    var res = this._getCollectionInfosCommand();
    if ( res ) {
        return res;
    }
    return this._getCollectionInfosSystemNamespaces();
}

/**
 * Returns this database's list of collection names in sorted order.
 */
DB.prototype.getCollectionNames = function() {
    return this.getCollectionInfos().map(function(infoObj) { return infoObj.name; });
}

DB.prototype.tojson = function(){
    return this._name;
}

DB.prototype.toString = function(){
    return this._name;
}

DB.prototype.isMaster = function () { return this.runCommand("isMaster"); }

DB.prototype.currentOp = function( arg ){
    var q = {}
    if ( arg ) {
        if ( typeof( arg ) == "object" )
            Object.extend( q , arg );
        else if ( arg )
            q["$all"] = true;
    }

    // don't send any read preference with psudo commands
    var _readPref = this.getMongo().getReadPrefMode();
    try {
        this.getMongo().setReadPref(null);
        var results = this.$cmd.sys.inprog.findOne( q );
    } finally {
        this.getMongo().setReadPref(_readPref);
    }

    return results
}
DB.prototype.currentOP = DB.prototype.currentOp;

DB.prototype.killOp = function(op) {
    if( !op ) 
        throw "no opNum to kill specified";

    var _readPref = this.getMongo().getReadPrefMode();
    try {
        this.getMongo().setReadPref(null);
        var results = this.$cmd.sys.killop.findOne({'op':op});
    } finally {
        this.getMongo().setReadPref(_readPref);
    }

    return results
}
DB.prototype.killOP = DB.prototype.killOp;

DB.tsToSeconds = function(x){
    if ( x.t && x.i )
        return x.t;
    return x / 4294967296; // low 32 bits are ordinal #s within a second
}

/** 
  Get a replication log information summary.
  <p>
  This command is for the database/cloud administer and not applicable to most databases.
  It is only used with the local database.  One might invoke from the JS shell:
  <pre>
       use local
       db.getReplicationInfo();
  </pre>
  It is assumed that this database is a replication master -- the information returned is 
  about the operation log stored at local.oplog.$main on the replication master.  (It also 
  works on a machine in a replica pair: for replica pairs, both machines are "masters" from 
  an internal database perspective.
  <p>
  * @return Object timeSpan: time span of the oplog from start to end  if slave is more out 
  *                          of date than that, it can't recover without a complete resync
*/
DB.prototype.getReplicationInfo = function() { 
    var db = this.getSiblingDB("local");

    var result = { };
    var oplog;
    if (db.system.namespaces.findOne({name:"local.oplog.rs"}) != null) {
        oplog = 'oplog.rs';
    }
    else if (db.system.namespaces.findOne({name:"local.oplog.$main"}) != null) {
        oplog = 'oplog.$main';
    }
    else {
        result.errmsg = "neither master/slave nor replica set replication detected";
        return result;
    }

    var ol_entry = db.system.namespaces.findOne({name:"local."+oplog});
    if( ol_entry && ol_entry.options ) {
        result.logSizeMB = ol_entry.options.size / ( 1024 * 1024 );
    } else {
        result.errmsg  = "local."+oplog+", or its options, not found in system.namespaces collection";
        return result;
    }
    ol = db.getCollection(oplog);

    result.usedMB = ol.stats().size / ( 1024 * 1024 );
    result.usedMB = Math.ceil( result.usedMB * 100 ) / 100;

    var firstc = ol.find().sort({$natural:1}).limit(1);
    var lastc = ol.find().sort({$natural:-1}).limit(1);
    if( !firstc.hasNext() || !lastc.hasNext() ) { 
        result.errmsg = "objects not found in local.oplog.$main -- is this a new and empty db instance?";
        result.oplogMainRowCount = ol.count();
        return result;
    }

    var first = firstc.next();
    var last = lastc.next();
    var tfirst = first.ts;
    var tlast = last.ts;

    if( tfirst && tlast ) { 
        tfirst = DB.tsToSeconds( tfirst ); 
        tlast = DB.tsToSeconds( tlast );
        result.timeDiff = tlast - tfirst;
        result.timeDiffHours = Math.round(result.timeDiff / 36)/100;
        result.tFirst = (new Date(tfirst*1000)).toString();
        result.tLast  = (new Date(tlast*1000)).toString();
        result.now = Date();
    }
    else { 
        result.errmsg = "ts element not found in oplog objects";
    }

    return result;
};

DB.prototype.printReplicationInfo = function() {
    var result = this.getReplicationInfo();
    if( result.errmsg ) {
        var isMaster = this.isMaster();
        if (isMaster.arbiterOnly) {
            print("cannot provide replication status from an arbiter.");
            return;
        }
        else if (!isMaster.ismaster) {
            print("this is a slave, printing slave replication info.");
            this.printSlaveReplicationInfo();
            return;
        }
        print(tojson(result));
        return;
    }
    print("configured oplog size:   " + result.logSizeMB + "MB");
    print("log length start to end: " + result.timeDiff + "secs (" + result.timeDiffHours + "hrs)");
    print("oplog first event time:  " + result.tFirst);
    print("oplog last event time:   " + result.tLast);
    print("now:                     " + result.now);
}

DB.prototype.printSlaveReplicationInfo = function() {
    var startOptimeDate = null;

    function getReplLag(st) {
        assert( startOptimeDate , "how could this be null (getReplLag startOptimeDate)" );
        print("\tsyncedTo: " + st.toString() );
        var ago = (startOptimeDate-st)/1000;
        var hrs = Math.round(ago/36)/100;
        print("\t" + Math.round(ago) + " secs (" + hrs + " hrs) behind the primary ");
    };

    function getMaster(members) {
        var found;
        members.forEach(function(row) {
            if (row.self) {
                found = row;
                return false;
            }
        });

        if (found) {
            return found;
        }
    };

    function g(x) {
        assert( x , "how could this be null (printSlaveReplicationInfo gx)" )
        print("source: " + x.host);
        if ( x.syncedTo ){
            var st = new Date( DB.tsToSeconds( x.syncedTo ) * 1000 );
            getReplLag(st);
        }
        else {
            print( "\tdoing initial sync" );
        }
    };

    function r(x) {
        assert( x , "how could this be null (printSlaveReplicationInfo rx)" );
        if ( x.state == 1 || x.state == 7 ) {  // ignore primaries (1) and arbiters (7)
            return;
        }
        
        print("source: " + x.name);
        if ( x.optime ) {
            getReplLag(x.optimeDate);
        }
        else {
            print( "\tno replication info, yet.  State: " + x.stateStr );
        }
    };
    
    var L = this.getSiblingDB("local");

    if (L.system.replset.count() != 0) {
        var status = this.adminCommand({'replSetGetStatus' : 1});
        startOptimeDate = getMaster(status.members).optimeDate; 
        status.members.forEach(r);
    }
    else if( L.sources.count() != 0 ) {
        startOptimeDate = new Date();
        L.sources.find().forEach(g);
    }
    else {
        print("local.sources is empty; is this db a --slave?");
        return;
    }
}

DB.prototype.serverBuildInfo = function(){
    return this._adminCommand( "buildinfo" );
}

DB.prototype.serverStatus = function( options ){
    var cmd = { serverStatus : 1 };
    if ( options ) {
        Object.extend( cmd, options );
    }
    return this._adminCommand( cmd );
}

DB.prototype.hostInfo = function(){
    return this._adminCommand( "hostInfo" );
}

DB.prototype.serverCmdLineOpts = function(){
    return this._adminCommand( "getCmdLineOpts" );
}

DB.prototype.version = function(){
    return this.serverBuildInfo().version;
}

DB.prototype.serverBits = function(){
    return this.serverBuildInfo().bits;
}

DB.prototype.listCommands = function(){
    var x = this.runCommand( "listCommands" );
    for ( var name in x.commands ){
        var c = x.commands[name];

        var s = name + ": ";

        if (c.adminOnly) s += " adminOnly ";
        if (c.adminOnly) s += " slaveOk ";

        s += "\n  ";
        s += c.help.replace(/\n/g, '\n  ');
        s += "\n";
        
        print( s );
    }
}

DB.prototype.printShardingStatus = function( verbose ){
    printShardingStatus( this.getSiblingDB( "config" ) , verbose );
}

DB.prototype.fsyncLock = function() {
    return this.adminCommand({fsync:1, lock:true});
}

DB.prototype.fsyncUnlock = function() {
    var _readPref = this.getMongo().getReadPrefMode();
    try {
        this.getMongo().setReadPref(null);
        var results = this.getSiblingDB("admin").$cmd.sys.unlock.findOne();
    } finally {
        this.getMongo().setReadPref(_readPref);
    }

    return results
}

DB.autocomplete = function(obj){
    var colls = obj.getCollectionNames();
    var ret=[];
    for (var i=0; i<colls.length; i++){
        if (colls[i].match(/^[a-zA-Z0-9_.\$]+$/))
            ret.push(colls[i]);
    }
    return ret;
}

DB.prototype.setSlaveOk = function( value ) {
    if( value == undefined ) value = true;
    this._slaveOk = value;
}

DB.prototype.getSlaveOk = function() {
    if (this._slaveOk != undefined) return this._slaveOk;
    return this._mongo.getSlaveOk();
}

/* Loads any scripts contained in system.js into the client shell.
*/
DB.prototype.loadServerScripts = function(){
    this.system.js.find().forEach(function(u){eval(u._id + " = " + u.value);});
}


////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////// Security shell helpers below //////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

var _defaultWriteConcern = { w: 'majority', wtimeout: 30 * 1000 }

function getUserObjString(userObj) {
    var pwd = userObj.pwd;
    delete userObj.pwd;
    var toreturn = tojson(userObj);
    userObj.pwd = pwd;
    return toreturn;
}

/**
 * Used for creating users in systems with v1 style user information (ie MongoDB v2.4 and prior)
 */
DB.prototype._addUserWithInsert = function(userObj, replicatedTo, timeout) {
    var c = this.getCollection( "system.users" );
    userObj = Object.extend({}, userObj); // Prevent modifications to userObj from getting to caller
    if (userObj.pwd != null) {
        userObj.pwd = _hashPassword(userObj.user, userObj.pwd);
    }

    try {
        c.save(userObj);
    } catch (e) {
        // SyncClusterConnections call GLE automatically after every write and will throw an
        // exception if the insert failed.
        if ( tojson(e).indexOf( "login" ) >= 0 ){
            // TODO: this check is a hack
            print( "Creating user seems to have succeeded but threw an exception because we no " +
                   "longer have auth." );
        } else {
            throw "Could not insert into system.users: " + tojson(e);
        }
    }
    print("Successfully added user: " + getUserObjString(userObj));

    //
    // When saving users to replica sets, the shell user will want to know if the user hasn't
    // been fully replicated everywhere, since this will impact security.  By default, replicate to
    // majority of nodes with wtimeout 15 secs, though user can override
    //
    
    replicatedTo = replicatedTo != undefined && replicatedTo != null ? replicatedTo : "majority"
    
    // in mongod version 2.1.0-, this worked
    var le = {};
    try {        
        le = this.getLastErrorObj( replicatedTo, timeout || 30 * 1000 );
        // printjson( le )
    }
    catch (e) {
        errjson = tojson(e);
        if ( errjson.indexOf( "login" ) >= 0 || errjson.indexOf( "unauthorized" ) >= 0 ) {
            // TODO: this check is a hack
            print( "addUser succeeded, but cannot wait for replication since we no longer have auth" );
            return "";
        }
        print( "could not find getLastError object : " + tojson( e ) )
    }

    if (!le.err) {
        return;
    }

    // We can't detect replica set shards via mongos, so we'll sometimes get this error
    // In this case though, we've already checked the local error before returning norepl, so
    // the user has been written and we're happy
    if (le.err == "norepl" || le.err == "noreplset") {
        // nothing we can do
        return;
    }

    if (le.err == "timeout") {
        throw "timed out while waiting for user authentication to replicate - " +
              "database will not be fully secured until replication finishes"
    }

    if (le.err.startsWith("E11000 duplicate key error")) {
        throw "User already exists with that username/userSource combination";
    }

    throw "couldn't add user: " + le.err;
}

/**
 * Helper method to convert "replicatedTo" and "timeout" fields into a writeConcern object
 */
DB.prototype._getWriteConcern = function(replicatedTo, timeout) {
    return { w: replicatedTo != null ? replicatedTo : _defaultWriteConcern.w,
             wtimeout: timeout || _defaultWriteConcern.wtimeout };
}

DB.prototype._addUser = function(userObj, replicatedTo, timeout) {
    if (typeof replicatedTo == "object") {
        throw Error("Cannot provide write concern object to addUser shell helper, " +
                    "use createUser instead");
    }
    var commandExisted = this._createUser(userObj, this._getWriteConcern(replicatedTo, timeout));
    if (!commandExisted) {
        this._addUserWithInsert(userObj, replicatedTo, timeout);
    }
}

DB.prototype._modifyCommandToDigestPasswordIfNecessary = function(cmdObj, username) {
    if (!cmdObj["pwd"]) {
        return;
    }
    if (cmdObj.hasOwnProperty("digestPassword")) {
        throw Error("Cannot specify 'digestPassword' through the user management shell helpers, " +
                    "use 'passwordDigestor' instead");
    }
    var passwordDigestor = cmdObj["passwordDigestor"] ? cmdObj["passwordDigestor"] : "client";
    if (passwordDigestor == "server") {
        cmdObj["digestPassword"] = true;
    } else if (passwordDigestor == "client") {
        cmdObj["pwd"] = _hashPassword(username, cmdObj["pwd"]);
        cmdObj["digestPassword"] = false;
    } else {
        throw Error("'passwordDigestor' must be either 'server' or 'client', got: '" +
                    passwordDigestor + "'");
    }
    delete cmdObj["passwordDigestor"];
}

// Returns true if it worked, false if the createUser command wasn't found, and throws on all other
// failures
DB.prototype._createUser = function(userObj, writeConcern) {
    var name = userObj["user"];
    var cmdObj = {createUser:name};
    cmdObj = Object.extend(cmdObj, userObj);
    delete cmdObj["user"];

    this._modifyCommandToDigestPasswordIfNecessary(cmdObj, name);

    cmdObj["writeConcern"] = writeConcern ? writeConcern : _defaultWriteConcern;

    var res = this.runCommand(cmdObj);

    if (res.ok) {
        print("Successfully added user: " + getUserObjString(userObj));
        return true;
    }

    if (res.errmsg == "no such cmd: createUser") {
        return false;
    }

    if (res.errmsg == "timeout") {
        throw Error("timed out while waiting for user authentication to replicate - " +
                    "database will not be fully secured until replication finishes");
    }

    throw Error("couldn't add user: " + res.errmsg);
}

function _hashPassword(username, password) {
    return hex_md5(username + ":mongo:" + password);
}

// We need to continue to support the addUser(username, password, readOnly) form of addUser for at
// least one release, even though its behavior of creating a super-user by default is bad.
// TODO(spencer): remove this form from v2.8
DB.prototype._addUserDeprecatedV22Version = function(username, pass, readOnly, replicatedTo, timeout) {
    if ( pass == null || pass.length == 0 )
        throw "password can't be empty";

    var userObjForCommand = { user: username, pwd: pass };
    if (this.getName() == "admin") {
        if (readOnly) {
            userObjForCommand["roles"] = ['readAnyDatabase'];
        } else {
            userObjForCommand["roles"] = ['root'];
        }
    } else {
        if (readOnly) {
            userObjForCommand["roles"] = ['read'];
        } else {
            userObjForCommand["roles"] = ['dbOwner'];
        }
    }

    var commandExisted = this._createUser(userObjForCommand,
                                          this._getWriteConcern(replicatedTo, timeout));
    if (!commandExisted) {
        var userObjForInsert = { user: username, pwd: pass, readOnly: readOnly || false };
        this._addUserWithInsert(userObjForInsert, replicatedTo, timeout);
    }
}

/**
 * TODO(spencer): Remove this from 2.8, in favor of "createUser"
 */
DB.prototype.addUser = function() {
    print("WARNING: The 'addUser' shell helper is DEPRECATED. Please use 'createUser' instead");

    if (arguments.length == 0) {
        throw Error("No arguments provided to addUser");
    }

    if (typeof arguments[0] == "object") {
        this._addUser.apply(this, arguments);
    } else {
        this._addUserDeprecatedV22Version.apply(this, arguments);
    }
}

DB.prototype.createUser = function(userObj, writeConcern) {
    var commandExisted = this._createUser(userObj, writeConcern);
    if (!commandExisted) {
        throw Error("'createUser' command not found.  This is most likely because you are " +
                    "talking to an old (pre v2.6) MongoDB server");
    }
}
/**
 * Used for updating users in systems with V1 style user information
 * (ie MongoDB v2.4 and prior)
 */
DB.prototype._updateUserV1 = function(name, updateObject, writeConcern) {
    var setObj = {};
    if (updateObject.pwd) {
        setObj["pwd"] = _hashPassword(name, updateObject.pwd);
    }
    if (updateObject.extraData) {
        setObj["extraData"] = updateObject.extraData;
    }
    if (updateObject.roles) {
        setObj["roles"] = updateObject.roles;
    }

    db.system.users.update({user : name, userSource : null},
                           {$set : setObj});
    var err = db.getLastError(writeConcern['w'], writeConcern['wtimeout']);
    if (err) {
        throw Error("Updating user failed: " + err);
    }
};

DB.prototype.updateUser = function(name, updateObject, writeConcern) {
    var cmdObj = {updateUser:name};
    cmdObj = Object.extend(cmdObj, updateObject);
    cmdObj['writeConcern'] =  writeConcern ? writeConcern : _defaultWriteConcern;
    this._modifyCommandToDigestPasswordIfNecessary(cmdObj, name);

    var res = this.runCommand(cmdObj);
    if (res.ok) {
        return;
    }

    if (res.errmsg == "no such cmd: updateUser") {
        this._updateUserV1(name, updateObject, cmdObj['writeConcern']);
        return;
    }

    throw Error("Updating user failed: " + res.errmsg);
};

DB.prototype.changeUserPassword = function(username, password, writeConcern) {
    this.updateUser(username, {pwd:password}, writeConcern);
};

DB.prototype.logout = function(){
    return this.getMongo().logout(this.getName());
};

// For backwards compatibility
DB.prototype.removeUser = function( username, writeConcern ) {
    print("WARNING: db.removeUser has been deprecated, please use db.dropUser instead");
    return db.dropUser(username, writeConcern);
}

DB.prototype.dropUser = function( username, writeConcern ){
    var cmdObj = {dropUser: username,
                  writeConcern: writeConcern ? writeConcern : _defaultWriteConcern};
    var res = this.runCommand(cmdObj);

    if (res.ok) {
        return true;
    }

    if (res.code == 11) { // Code 11 = UserNotFound
        return false;
    }

    if (res.errmsg == "no such cmd: dropUsers") {
        return this._removeUserV1(username, cmdObj['writeConcern']);
    }

    throw Error(res.errmsg);
}

/**
 * Used for removing users in systems with V1 style user information
 * (ie MongoDB v2.4 and prior)
 */
DB.prototype._removeUserV1 = function(username, writeConcern) {
    this.getCollection( "system.users" ).remove( { user : username } );

    var le = db.getLastErrorObj(writeConcern['w'], writeConcern['wtimeout']);

    if (le.err) {
        throw "Couldn't remove user: " + le.err;
    }

    if (le.n == 1) {
        return true;
    } else {
        return false;
    }
}

DB.prototype.dropAllUsers = function(writeConcern) {
    var res = this.runCommand({dropAllUsersFromDatabase:1,
                               writeConcern: writeConcern ? writeConcern : _defaultWriteConcern});

    if (!res.ok) {
        throw Error(res.errmsg);
    }

    return res.n;
}

DB.prototype.__pwHash = function( nonce, username, pass ) {
    return hex_md5(nonce + username + _hashPassword(username, pass));
}

DB.prototype._defaultAuthenticationMechanism = "MONGODB-CR";
DB.prototype._defaultGssapiServiceName = null;

DB.prototype._authOrThrow = function () {
    var params;
    if (arguments.length == 2) {
        params = { user: arguments[0], pwd: arguments[1] };
    }
    else if (arguments.length == 1) {
        if (typeof(arguments[0]) != "object")
            throw Error("Single-argument form of auth expects a parameter object");
        params = Object.extend({}, arguments[0]);
    }
    else {
        throw Error(
            "auth expects either (username, password) or ({ user: username, pwd: password })");
    }

    if (params.mechanism === undefined)
        params.mechanism = this._defaultAuthenticationMechanism;

    if (params.db !== undefined) {
        throw Error("Do not override db field on db.auth(). Use getMongo().auth(), instead.");
    }

    if (params.mechanism == "GSSAPI" &&
        params.serviceName == null &&
        this._defaultGssapiServiceName != null) {

        params.serviceName = this._defaultGssapiServiceName;
    }

    params.db = this.getName();
    var good = this.getMongo().auth(params);
    if (good) {
        // auth enabled, and should try to use isMaster and replSetGetStatus to build prompt
        this.getMongo().authStatus = {authRequired:true, isMaster:true, replSetGetStatus:true};
    }

    return good;
}


DB.prototype.auth = function() {
    var ex;
    try {
        this._authOrThrow.apply(this, arguments);
    } catch (ex) {
        print(ex);
        return 0;
    }
    return 1;
}

DB.prototype.grantRolesToUser = function(username, roles, writeConcern) {
    var cmdObj = {grantRolesToUser: username,
                  roles: roles,
                  writeConcern: writeConcern ? writeConcern : _defaultWriteConcern};
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw Error(res.errmsg);
    }
}

DB.prototype.revokeRolesFromUser = function(username, roles, writeConcern) {
    var cmdObj = {revokeRolesFromUser: username,
                  roles: roles,
                  writeConcern: writeConcern ? writeConcern : _defaultWriteConcern};
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw Error(res.errmsg);
    }
}

DB.prototype.getUser = function(username, args) {
    if (typeof username != "string") {
        throw Error("User name for getUser shell helper must be a string");
    }
    var cmdObj = {usersInfo: username};
    Object.extend(cmdObj, args);

    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw Error(res.errmsg);
    }

    if (res.users.length == 0) {
        return null;
    }
    return res.users[0];
}

DB.prototype.getUsers = function(args) {
    var cmdObj = {usersInfo: 1};
    Object.extend(cmdObj, args);
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        var authSchemaIncompatibleCode = 69;
        if (res.code == authSchemaIncompatibleCode ||
                (res.code == null && res.errmsg == "no such cmd: usersInfo")) {
            // Working with 2.4 schema user data
            return this.system.users.find({}).toArray();
        }

        throw Error(res.errmsg);
    }

    return res.users;
}

DB.prototype.createRole = function(roleObj, writeConcern) {
    var name = roleObj["role"];
    var cmdObj = {createRole:name};
    cmdObj = Object.extend(cmdObj, roleObj);
    delete cmdObj["role"];
    cmdObj["writeConcern"] = writeConcern ? writeConcern : _defaultWriteConcern;

    var res = this.runCommand(cmdObj);

    if (!res.ok) {
        throw Error(res.errmsg);
    }
    printjson(roleObj);
}

DB.prototype.updateRole = function(name, updateObject, writeConcern) {
    var cmdObj = {updateRole:name};
    cmdObj = Object.extend(cmdObj, updateObject);
    cmdObj['writeConcern'] =  writeConcern ? writeConcern : _defaultWriteConcern;
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw Error(res.errmsg);
    }
};

DB.prototype.dropRole = function(name, writeConcern) {
    var cmdObj = {dropRole:name,
                  writeConcern: writeConcern ? writeConcern : _defaultWriteConcern};
    var res = this.runCommand(cmdObj);

    if (res.ok) {
        return true;
    }

    if (res.code == 31) { // Code 31 = RoleNotFound
        return false;
    }

    throw Error(res.errmsg);
};

DB.prototype.dropAllRoles = function(writeConcern) {
    var res = this.runCommand({dropAllRolesFromDatabase:1,
                               writeConcern: writeConcern ? writeConcern : _defaultWriteConcern});

    if (!res.ok) {
        throw Error(res.errmsg);
    }

    return res.n;
}

DB.prototype.grantRolesToRole = function(rolename, roles, writeConcern) {
    var cmdObj = {grantRolesToRole: rolename,
                  roles: roles,
                  writeConcern: writeConcern ? writeConcern : _defaultWriteConcern};
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw Error(res.errmsg);
    }
}

DB.prototype.revokeRolesFromRole = function(rolename, roles, writeConcern) {
    var cmdObj = {revokeRolesFromRole: rolename,
                  roles: roles,
                  writeConcern: writeConcern ? writeConcern : _defaultWriteConcern};
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw Error(res.errmsg);
    }
}

DB.prototype.grantPrivilegesToRole = function(rolename, privileges, writeConcern) {
    var cmdObj = {grantPrivilegesToRole: rolename,
                  privileges: privileges,
                  writeConcern: writeConcern ? writeConcern : _defaultWriteConcern};
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw Error(res.errmsg);
    }
}

DB.prototype.revokePrivilegesFromRole = function(rolename, privileges, writeConcern) {
    var cmdObj = {revokePrivilegesFromRole: rolename,
                  privileges: privileges,
                  writeConcern: writeConcern ? writeConcern : _defaultWriteConcern};
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw Error(res.errmsg);
    }
}

DB.prototype.getRole = function(rolename, args) {
    if (typeof rolename != "string") {
        throw Error("Role name for getRole shell helper must be a string");
    }
    var cmdObj = {rolesInfo:rolename};
    Object.extend(cmdObj, args);
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw Error(res.errmsg);
    }

    if (res.roles.length == 0) {
        return null;
    }
    return res.roles[0];
}

DB.prototype.getRoles = function(args) {
    var cmdObj = {rolesInfo:1};
    Object.extend(cmdObj, args);
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw Error(res.errmsg);
    }

    return res.roles;
}

DB.prototype.setWriteConcern = function( wc ) {
    if ( wc instanceof WriteConcern ) {
        this._writeConcern = wc;
    }
    else {
        this._writeConcern = new WriteConcern( wc );
    }
};

DB.prototype.getWriteConcern = function() {
    if (this._writeConcern)
        return this._writeConcern;
    
    if (this._mongo.getWriteConcern())
        return this._mongo.getWriteConcern();

    return null;
};

DB.prototype.unsetWriteConcern = function() {
    delete this._writeConcern;
};


}());
