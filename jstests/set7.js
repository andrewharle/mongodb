// test $set with array indicies

t = db.jstests_set7;

t.drop();

t.save( {a:[0,1,2,3]} );
t.update( {}, {$set:{"a.0":2}} );
assert.eq( [2,1,2,3], t.findOne().a );

t.update( {}, {$set:{"a.4":5}} );
assert.eq( [2,1,2,3,5], t.findOne().a );

t.update( {}, {$set:{"a.9":9}} );
assert.eq( [2,1,2,3,5,null,null,null,null,9], t.findOne().a );

t.drop();
t.save( {a:[0,1,2,3]} );
t.update( {}, {$set:{"a.9":9,"a.7":7}} );
assert.eq( [0,1,2,3,null,null,null,7,null,9], t.findOne().a );

t.drop();
t.save( {a:[0,1,2,3,4,5,6,7,8,9,10]} );
t.update( {}, {$set:{"a.11":11} } );
assert.eq( [0,1,2,3,4,5,6,7,8,9,10,11], t.findOne().a );

t.drop();
t.save( {} );
t.update( {}, {$set:{"a.0":4}} );
assert.eq( {"0":4}, t.findOne().a );

t.drop();
t.update( {"a.0":4}, {$set:{b:1}}, true );
assert.eq( {"0":4}, t.findOne().a );

t.drop();
t.save( {a:[]} );
t.update( {}, {$set:{"a.f":1}} );
assert( db.getLastError() );
assert.eq( [], t.findOne().a );
