
t = db.mr_bigobject
t.drop()

s = "";
while ( s.length < ( 1024 * 1024 ) ){
    s += "asdasdasd";
}

for ( i=0; i<10; i++ )
    t.insert( { _id : i , s : s } )

m = function(){
    emit( 1 , this.s + this.s );
}

r = function( k , v ){
    return 1;
}

assert.throws( function(){ t.mapReduce( m , r ); } , "emit should fail" )

m = function(){
    emit( 1 , this.s );
}

assert.eq( { 1 : 1 } , t.mapReduce( m , r ).convertToSingleObject() , "A1" )

r = function( k , v ){
    total = 0;
    for ( var i=0; i<v.length; i++ ){
        var x = v[i];
        if ( typeof( x ) == "number" )
            total += x
        else
            total += x.length;
    }
    return total;
}

assert.eq( { 1 : 10 * s.length } , t.mapReduce( m , r ).convertToSingleObject() , "A1" )
