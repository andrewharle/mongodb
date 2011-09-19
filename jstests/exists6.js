// SERVER-393 Test indexed matching with $exists.

t = db.jstests_exists6;
t.drop();

t.ensureIndex( {b:1} );
t.save( {} );
t.save( {b:1} );
t.save( {b:null} );

checkExists = function( query ) {
    // Constraint on 'b' is trivial, so a BasicCursor is the default cursor type.
    assert.eq( 'BasicCursor', t.find( query ).explain().cursor );
    // Index bounds include all elements.
    assert.eq( [ [ { $minElement:1 }, { $maxElement:1 } ] ], t.find( query ).hint( {b:1} ).explain().indexBounds.b );
    // All keys must be scanned.
    assert.eq( 3, t.find( query ).hint( {b:1} ).explain().nscanned );
    // 2 docs will match.
    assert.eq( 2, t.find( query ).hint( {b:1} ).itcount() );    
}
checkExists( {b:{$exists:true}} );
checkExists( {b:{$not:{$exists:false}}} );

checkMissing = function( query ) {
    // Constraint on 'b' is nontrivial, so a BtreeCursor is the default cursor type.
    assert.eq( 'BtreeCursor b_1', t.find( query ).explain().cursor );
    // Scan null index keys.
    assert.eq( [ [ null, null ] ], t.find( query ).explain().indexBounds.b );
    // Two existing null keys will be scanned.
    assert.eq( 2, t.find( query ).explain().nscanned );
    // One doc is missing 'b'.
    assert.eq( 1, t.find( query ).hint( {b:1} ).itcount() );    
}
checkMissing( {b:{$exists:false}} );
checkMissing( {b:{$not:{$exists:true}}} );

// Now check existence of second compound field.
t.ensureIndex( {a:1,b:1} );
t.save( {a:1} );
t.save( {a:1,b:1} );
t.save( {a:1,b:null} );

checkExists = function( query ) {
    // Index bounds include all elements.
    assert.eq( [ [ { $minElement:1 }, { $maxElement:1 } ] ], t.find( query ).explain().indexBounds.b );
    // All keys must be scanned.
    assert.eq( 3, t.find( query ).explain().nscanned );
    // 2 docs will match.
    assert.eq( 2, t.find( query ).hint( {a:1,b:1} ).itcount() );    
}
checkExists( {a:1,b:{$exists:true}} );
checkExists( {a:1,b:{$not:{$exists:false}}} );

checkMissing = function( query ) {
    // Scan null index keys.
    assert.eq( [ [ null, null ] ], t.find( query ).explain().indexBounds.b );
    // Two existing null keys will be scanned.
    assert.eq( 2, t.find( query ).explain().nscanned );
    // One doc is missing 'b'.
    assert.eq( 1, t.find( query ).hint( {a:1,b:1} ).itcount() );    
}
checkMissing( {a:1,b:{$exists:false}} );
checkMissing( {a:1,b:{$not:{$exists:true}}} );
