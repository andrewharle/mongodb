/* usage:
$ ./replay_test.sh \
      -w testWorkloads/conc1.js \
      -w testWorkloads/conc2.js \
      -a 'db.bench.count() == 6000' \
      -a 'db.bench.find({b:true}).count() == 2500' \
      -a 'db.bench.find({c:true}).count() == 2500'
*/
// to be used with conc2
start = 0;
for (i=start; i<1000; i+=2) {
  docs = [];
  for (j=0; j<10; j++) {
    docs.push({a: i*10+j});
  }
  db.bench.insert(docs);
}
for (i=start; i<1000; i+=2) {
  for (j=0; j<10; j+=2) {
    db.bench.deleteOne({a: i*10+j});
  }
  for (j=1; j<10; j+=2) {
    db.bench.updateOne({a: i*10+j}, {$set:{b:true}});
  }
}
// idempotent overlap
for (i=0; i<1000; i++) {
  db.bench.insert({_id:i, r:i, s:i});
  db.bench.updateOne({_id:i}, {$set:{r:i+1}, $set:{s:i+2}})
}
