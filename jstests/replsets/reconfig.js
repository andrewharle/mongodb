
// try reconfiguring with servers down

var replTest = new ReplSetTest({ name: 'testSet', nodes: 5 });
var nodes = replTest.startSet();
replTest.initiate();

var master = replTest.getMaster();

print("initial sync");
master.getDB("foo").bar.insert({X:1});
replTest.awaitReplication();

print("stopping 3 & 4");
replTest.stop(3);
replTest.stop(4);

print("reconfiguring");
master = replTest.getMaster();
var config = master.getDB("local").system.replset.findOne();
var oldVersion = config.version++;
config.members[0].votes = 2;
config.members[3].votes = 2;
try {
    master.getDB("admin").runCommand({replSetReconfig : config});
}
catch(e) {
    print(e);
}

var config = master.getDB("local").system.replset.findOne();
assert.eq(oldVersion+1, config.version);


print("0 & 3 up; 1, 2, 4 down");
replTest.restart(3);

// in case 0 isn't master
replTest.awaitReplication();

replTest.stop(1);
replTest.stop(2);

print("try to reconfigure with a 'majority' down");
oldVersion = config.version;
config.version++;
master = replTest.getMaster();
try {
    master.getDB("admin").runCommand({replSetReconfig : config});
}
catch (e) {
    print(e);
}

var config = master.getDB("local").system.replset.findOne();
assert.eq(oldVersion+1, config.version);

replTest.stopSet();

replTest2 = new ReplSetTest({name : 'testSet2', nodes : 1});
nodes = replTest2.startSet();

result = nodes[0].getDB("admin").runCommand({replSetInitiate : {_id : "testSet2", members : [
    {_id : 0, tags : ["member0"]}
   ]}});

assert(result.errmsg.match(/bad or missing host field/));

replTest2.stopSet();
