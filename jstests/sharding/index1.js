// from server 2326 - make sure that sharding only works with unique indices

s = new ShardingTest( "shard_index", 2, 50, 1 )

// Regenerate fully because of SERVER-2782
for ( var i = 0; i < 10; i++ ) {
		
	var coll = s.admin._mongo.getDB( "test" ).getCollection( "foo" + i )
	coll.drop()

	for ( var j = 0; j < 300; j++ ) {
		coll.insert( { num : j, x : 1 } )
	}

	if(i == 0) s.adminCommand( { enablesharding : "" + coll._db } );

	print("\n\n\n\n\nTest # " + i)
	
	if ( i == 0 ) {

		// Unique index exists, but not the right one.
		coll.ensureIndex( { num : 1 }, { unique : true } )
		coll.ensureIndex( { x : 1 } )

		passed = false
		try {
			s.adminCommand( { shardcollection : "" + coll, key : { x : 1 } } )
			passed = true
		} catch (e) {
			print( e )
		}
		assert( !passed, "Should not shard collection when another unique index exists!")

	}
	if ( i == 1 ) {
		
		// Unique index exists as prefix, also index exists
		coll.ensureIndex( { x : 1 } )
		coll.ensureIndex( { x : 1, num : 1 }, { unique : true } )
		
		try{
			s.adminCommand({ shardcollection : "" + coll, key : { x : 1 } })
		}
		catch(e){
			print(e)
			assert( false, "Should be able to shard non-unique index without unique option.")
		}
		
	}
	if ( i == 2 ) {
        if (false) { // SERVER-3718
		// Non-unique index exists as prefix, also index exists.  No unique index.
		coll.ensureIndex( { x : 1 } )
		coll.ensureIndex( { x : 1, num : 1 } )

        passed = false;
		try{
			s.adminCommand({ shardcollection : "" + coll, key : { x : 1 } })
            passed = true;

		}
		catch( e ){
			print(e)
		}
        assert( !passed, "Should not shard collection with no unique index.")
        }
	}
	if ( i == 3 ) {

		// Unique index exists as prefix, also unique index exists
		coll.ensureIndex( { num : 1 }, { unique : true })
		coll.ensureIndex( { num : 1 , x : 1 }, { unique : true } )

		try{
			s.adminCommand({ shardcollection : "" + coll, key : { num : 1 }, unique : true })
		}
		catch( e ){
			print(e)
			assert( false, "Should be able to shard collection with unique prefix index.")
		}

	}
	if ( i == 4 ) {

		// Unique index exists as id, also unique prefix index exists
		coll.ensureIndex( { _id : 1, num : 1 }, { unique : true } )

		try{
			s.adminCommand({ shardcollection : "" + coll, key : { _id : 1 }, unique : true })
		}
		catch( e ){
			print(e)
			assert( false, "Should be able to shard collection with unique id index.")
		}
		
	}
	if ( i == 5 ) {

		// Unique index exists as id, also unique prefix index exists
		coll.ensureIndex( { _id : 1, num : 1 }, { unique : true } )

		try{
			s.adminCommand({ shardcollection : "" + coll, key : { _id : 1, num : 1 }, unique : true })
		}
		catch( e ){
			print(e)
			assert( false, "Should be able to shard collection with unique combination id index.")
		}
		
	}
	if ( i == 6 ) {

		coll.remove()
		
		// Unique index does not exist, also unique prefix index exists
		coll.ensureIndex( { num : 1, _id : 1 }, { unique : true } )

		try{
			s.adminCommand({ shardcollection : "" + coll, key : { num : 1 }, unique : true })
		}
		catch( e ){
			print(e)
			assert( false, "Should be able to shard collection with no unique index but with a unique prefix index.")
		}
		
	}
    if ( i == 7 ) {
		coll.remove()

		// No index exists

		try{
			s.adminCommand({ shardcollection : "" + coll, key : { num : 1 } })
		}
		catch( e ){
			print(e)
            assert( !passed, "Should be able to shard collection with no index on shard key.")
		}
	}
    if ( i == 8 ) {
        if (false) { // SERVER-3718
		coll.remove()

		// No index exists

        passed = false
		try{
			s.adminCommand({ shardcollection : "" + coll, key : { num : 1 }, unique : true })
            passed = true
		}
		catch( e ){
			print(e)
		}
        assert( !passed, "Should not shard collection with unique flag but with no unique index on shard key.")
        }
	}
    if ( i == 9 ) {

		// Unique index exists on a different field as well
		coll.ensureIndex( { num : 1 }, { unique : true } )
		coll.ensureIndex( { x : 1 }, { unique : true} )

        passed = false
		try {
			s.adminCommand( { shardcollection : "" + coll, key : { x : 1 } } )
            passed = true
		} catch (e) {
			print( e )
		}
        assert( !passed, "Should not shard collection when another unique index exists!" )
	}
}

s.stop();
