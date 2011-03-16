// @file replsetarb3.js
// try turning arbiters into non-arbiters and vice versa

/*
 * 1: initialize set
 * 2: check m3.state == 7
 * 3: reconfig
 * 4: check m3.state == 2
 * 5: reconfig
 * 6: check m3.state == 7
 * 7: reconfig
 * 8: check m3.state == 2
 * 9: insert 10000
 * 10: reconfig
 * 11: check m3.state == 7
 */

var debug = false;

var statusSoon = function(s) {
  assert.soon(function() {
      var status = master.getDB("admin").runCommand({ replSetGetStatus: 1 });
      if (debug)
        printjson(status);
      return status.members[2].state == s;
    });
};

var w = 0;
var wait = function(f) {
    w++;
    var n = 0;
    while (!f()) {
        if( n % 4 == 0 )
            print("toostale.js waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        assert(n < 200, 'tried 200 times, giving up');
        sleep(1000);
    }
}

var reconnect = function(a) {
  wait(function() { 
      try {
        a.getDB("foo").bar.stats();
        return true;
      } catch(e) {
        print(e);
        return false;
      }
    });
};

var reconfig = function() {
  config.version++;
  try {
    var result = master.getDB("admin").runCommand({replSetReconfig : config});
  }
  catch(e) {
    print(e);
  }
  reconnect(master);
  reconnect(replTest.liveNodes.slaves[1]);
  sleep(20000);
};

var replTest = new ReplSetTest( {name: 'unicomplex', nodes: 3} );
var nodes = replTest.nodeList();

print(tojson(nodes));


var conns = replTest.startSet();

print("1");
var config = {"_id" : "unicomplex", "members" : [
    {"_id" : 0, "host" : nodes[0] },
    {"_id" : 1, "host" : nodes[1] },
    {"_id" : 2, "host" : nodes[2], "arbiterOnly" : true}]};
var r = replTest.initiate(config);
config.version = 1;

var master = replTest.getMaster();

// Wait for initial replication
master.getDB("foo").foo.insert({a: "foo"});
replTest.awaitReplication();


print("2");
statusSoon(7);
assert.eq(replTest.liveNodes.slaves[1].getDB("local").oplog.rs.count(), 0);

/*
print("3");
delete config.members[2].arbiterOnly;
reconfig();


print("4");
statusSoon(2);
assert(replTest.liveNodes.slaves[1].getDB("local").oplog.rs.count() > 0);


print("5");
config.members[2].arbiterOnly = true;
reconfig();


print("6");
statusSoon(7);
assert.eq(replTest.liveNodes.slaves[1].getDB("local").oplog.rs.count(), 0);


print("7");
delete config.members[2].arbiterOnly;
reconfig();


print("8");
statusSoon(2);
assert(replTest.liveNodes.slaves[1].getDB("local").oplog.rs.count() > 0);


print("9");
for (var i = 0; i < 10000; i++) {
  master.getDB("foo").bar.insert({increment : i, c : 0, foo : "kasdlfjaklsdfalksdfakldfmalksdfmaklmfalkfmkafmdsaklfma", date : new Date(), d : Date()});
}


print("10");
config.members[2].arbiterOnly = true;
reconfig();


print("11");
statusSoon(7);
assert.eq(replTest.liveNodes.slaves[1].getDB("local").oplog.rs.count(), 0);
*/

replTest.stopSet( 15 );

