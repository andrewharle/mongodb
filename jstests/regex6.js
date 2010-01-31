// contributed by Andrew Kempe
t = db.regex6;
t.drop();

t.save( { name : "eliot" } );
t.save( { name : "emily" } );
t.save( { name : "bob" } );
t.save( { name : "aaron" } );

t.ensureIndex( { name : 1 } );

assert.eq( 0 , t.find( { name : /^\// } ).count() , "index count" );
assert.eq( 0 , t.find( { name : /^\// } ).explain().nscanned , "index explain" );
assert.eq( 0 , t.find( { name : /^é/ } ).explain().nscanned , "index explain" );
assert.eq( 0 , t.find( { name : /^\é/ } ).explain().nscanned , "index explain" );
assert.eq( 0 , t.find( { name : /^\./ } ).explain().nscanned , "index explain" );
assert.eq( 4 , t.find( { name : /^./ } ).explain().nscanned , "index explain" );

assert.eq( 4 , t.find( { name : /^\Qblah\E/ } ).explain().nscanned , "index explain" );
