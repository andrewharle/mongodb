
t = db.index_elemmatch1
t.drop()

x = 0
y = 0
for ( a=0; a<100; a++ ){
    for ( b=0; b<100; b++ ){
        t.insert( { a : a , b : b % 10 , arr : [ { x : x++ % 10 , y : y++ % 10 } ] } )
    }
}

t.ensureIndex( { a : 1 , b : 1 } )
t.ensureIndex( { "arr.x" : 1 , a : 1 } )

assert.eq( 100 , t.find( { a : 55 } ).itcount() , "A1" );
assert.eq( 10 , t.find( { a : 55 , b : 7 } ).itcount() , "A2" );

q = { a : 55 , b : { $in : [ 1 , 5 , 8 ] } }
assert.eq( 30 , t.find( q ).itcount() , "A3" )

q.arr = { $elemMatch : { x : 5 , y : 5 } }
assert.eq( 10 , t.find( q ).itcount() , "A4" )

assert.eq( t.find(q).itcount() , t.find(q).explain().nscanned , "A5" )



