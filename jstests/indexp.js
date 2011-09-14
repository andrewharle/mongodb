// Check recording and playback of good query plans with different index types SERVER-958.

t = db.jstests_indexp;
t.drop();

function expectRecordedPlan( query, idx ) {
 	assert.eq( "BtreeCursor " + idx, t.find( query ).explain( true ).oldPlan.cursor );
}

function expectNoRecordedPlan( query ) {
 	assert.isnull( t.find( query ).explain( true ).oldPlan );
}

// Basic test
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:1} );
t.find( {a:1} ).itcount();
expectRecordedPlan( {a:1}, "a_1" );

// Index type changes
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:1} );
t.find( {a:1} ).itcount();
t.save( {a:[1,2]} );
expectRecordedPlan( {a:1}, "a_1" );

// Multi key QueryPattern reuses index
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:[1,2]} );
t.find( {a:{$gt:0}} ).itcount();
expectRecordedPlan( {a:{$gt:0,$lt:5}}, "a_1" );

// Single key QueryPattern can still be used to find best plan - at least for now.
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:1} );
t.find( {a:{$gt:0,$lt:5}} ).itcount();
t.save( {a:[1,2]} );
expectRecordedPlan( {a:{$gt:0,$lt:5}}, "a_1" );

// Invalid query with only valid fields used 
if ( 0 ) { // SERVER-2864
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:1}  );
t.find( {a:1,b:{$gt:5,$lt:0}} ).itcount();
expectRecordedPlan( {a:{$gt:0,$lt:5}}, "a_1" );
}

// Dummy query plan not stored
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:1} );
t.find( {a:{$gt:5,$lt:0}} ).itcount();
expectNoRecordedPlan( {a:{$gt:5,$lt:0}} );