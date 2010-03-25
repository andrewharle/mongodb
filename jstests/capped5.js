
tn = "capped5"

t = db[tn]
t.drop();

db.createCollection( tn , {capped: true, size: 1024 * 1024 * 1 } );
t.insert( { _id : 5 , x : 11 , z : 52 } );

assert.eq( 0 , t.getIndexKeys().length , "A0" )
assert.eq( 52 , t.findOne( { x : 11 } ).z , "A1" );
assert.eq( 52 , t.findOne( { _id : 5 } ).z , "A2" );

t.ensureIndex( { _id : 1 } )
t.ensureIndex( { x : 1 } )

assert.eq( 52 , t.findOne( { x : 11 } ).z , "B1" );
assert.eq( 52 , t.findOne( { _id : 5 } ).z , "B2" );

t.drop();
db.createCollection( tn , {capped: true, size: 1024 * 1024 * 1 } );
t.insert( { _id : 5 , x : 11 } );
t.insert( { _id : 6 , x : 11 } );
t.ensureIndex( { x:1 }, {unique:true, dropDups:true } );
assert.eq( 0, db.system.indexes.count( {ns:"test."+tn} ) );
assert.eq( 2, t.find().toArray().length );

t.drop();
db.createCollection( tn , {capped: true, size: 1024 * 1024 * 1 } );
t.insert( { _id : 5 , x : 11 } );
t.insert( { _id : 5 , x : 12 } );
t.ensureIndex( { _id:1 } );
assert.eq( 0, db.system.indexes.count( {ns:"test."+tn} ) );
assert.eq( 2, t.find().toArray().length );

t.drop();
db.createCollection( tn , {capped: true, size: 1024 * 1024 * 1 } );
t.insert( { _id : 5 , x : 11 } );
t.insert( { _id : 6 , x : 12 } );
t.ensureIndex( { x:1 }, {unique:true, dropDups:true } );
assert.eq( 1, db.system.indexes.count( {ns:"test."+tn} ) );
assert.eq( 2, t.find().hint( {x:1} ).toArray().length );

// SERVER-525
t.drop();
db.createCollection( tn , {capped: true, size: 1024 * 1024 * 1 } );
t.ensureIndex( { _id:1 } );
t.insert( { _id : 5 , x : 11 } );
t.insert( { _id : 5 , x : 12 } );
assert.eq( 1, t.find().toArray().length );
