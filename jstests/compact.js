// compact.js

t = db.compacttest;
t.drop();
t.insert({ x: 3 });
t.insert({ x: 3 });
t.insert({ x: 5 });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.ensureIndex({ x: 1 });

print("1");

var res = db.runCommand({ compact: 'compacttest', dev: true });
printjson(res);
assert(res.ok);
assert(t.count() == 9);
var v = t.validate(true);
assert(v.ok);
assert(v.extentCount == 1);
assert(v.deletedCount == 1);
assert(t.getIndexes().length == 2);

print("2");

// works on an empty collection?
t.remove({});
assert(db.runCommand({ compact: 'compacttest', dev: true }).ok);
assert(t.count() == 0);
v = t.validate(true);
assert(v.ok);
assert(v.extentCount == 1);
assert(t.getIndexes().length == 2);
