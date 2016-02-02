(function() {
    "use strict";
    var t = db.apply_ops1;
    t.drop();

    //
    // Input validation tests
    //

    // Empty array of operations.
    assert.commandWorked(db.adminCommand({applyOps: []}),
                         'applyOps should not fail on empty array of operations');

    // Non-array type for operations.
    assert.commandFailed(db.adminCommand({applyOps: "not an array"}),
                         'applyOps should fail on non-array type for operations');

    // Missing 'op' field in an operation.
    assert.commandFailed(db.adminCommand({applyOps: [{ns: t.getFullName()}]}),
                         'applyOps should fail on operation without "op" field');

    // Non-string 'op' field in an operation.
    assert.commandFailed(db.adminCommand({applyOps: [{op: 12345, ns: t.getFullName()}]}),
                         'applyOps should fail on operation with non-string "op" field');

    // Empty 'op' field value in an operation.
    assert.commandFailed(db.adminCommand({applyOps: [{op: '', ns: t.getFullName()}]}),
                         'applyOps should fail on operation with empty "op" field value');

    // Missing 'ns' field in an operation.
    assert.commandFailed(db.adminCommand({applyOps: [{op: 'c'}]}),
                         'applyOps should fail on operation without "ns" field');

    // Non-string 'ns' field in an operation.
    assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: 12345}]}),
                         'applyOps should fail on operation with non-string "ns" field');

    // Empty 'ns' field value in an operation of type 'n' (noop).
    assert.commandWorked(db.adminCommand({applyOps: [{op: 'n', ns: ''}]}),
                         'applyOps should work on no op operation with empty "ns" field value');

    // Missing 'o' field value in an operation of type 'c' (command).
    assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: 'foo'}]}),
                         'applyOps should fail on command operation without "o" field');

    // Non-object 'o' field value in an operation of type 'c' (command).
    assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: 'foo', o: 'bar'}]}),
                         'applyOps should fail on command operation with non-object "o" field');

    // Empty object 'o' field value in an operation of type 'c' (command).
    assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: 'foo', o: {}}]}),
                         'applyOps should fail on command operation with empty object "o" field');

    // Unknown key in 'o' field value in an operation of type 'c' (command).
    assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: 'foo', o: {a: 1}}]}),
                         'applyOps should fail on command operation on unknown key in "o" field');

    // Empty 'ns' field value in operation type other than 'n'.
    assert.commandFailed(
      db.adminCommand({applyOps: [{op: 'c', ns: ''}]}),
      'applyOps should fail on non-"n" operation type with empty "ns" field value'
    );

    // Valid 'ns' field value in unknown operation type 'x'.
    assert.commandFailed(
      db.adminCommand({applyOps: [{op: 'x', ns: t.getFullName()}]}),
      'applyOps should fail on unknown operation type "x" with valid "ns" value'
    );

    assert.eq(0, t.find().count() , "Non-zero amount of documents in collection to start");
    assert.commandFailed(db.adminCommand(
      {applyOps: [{"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 17}}]}
    ),
        "Applying an insert operation on a non-existent collection should fail");

    assert.commandWorked(db.createCollection(t.getName()));
    var a = db.adminCommand(
      {applyOps: [{"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 17}}]}
    );
    assert.eq(1, t.find().count() , "Valid insert failed");
    assert.eq(true, a.results[0], "Bad result value for valid insert");

    a = assert.commandWorked(db.adminCommand(
            {applyOps: [{"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 17}}]}
    ));
    assert.eq(1, t.find().count() , "Duplicate insert failed");
    assert.eq(true, a.results[0], "Bad result value for duplicate insert");

    var o = {_id: 5, x: 17};
    assert.eq(o , t.findOne() , "Mismatching document inserted.");

    var res = db.runCommand({applyOps: [
        {op: "u", ns: t.getFullName(), o2: { _id : 5 }, o: {$inc: {x: 1}}},
        {op: "u", ns: t.getFullName(), o2: { _id : 5 }, o: {$inc: {x: 1}}}
    ]});

    o.x++;
    o.x++;

    assert.eq(1, t.find().count(), "Updates increased number of documents");
    assert.eq(o, t.findOne(), "Document doesn't match expected");
    assert.eq(true, res.results[0], "Bad result value for valid update");
    assert.eq(true, res.results[1], "Bad result value for valid update");

    //preCondition fully matches
    res = db.runCommand({applyOps:
                            [
                                {op: "u", ns: t.getFullName(), o2: {_id : 5}, o: {$inc: {x :1}}},
                                {op: "u", ns: t.getFullName(), o2: {_id : 5}, o: {$inc: {x :1}}}
                            ],
                            preCondition: [{ns : t.getFullName(), q: {_id: 5}, res:{x: 19}}]
                        });

    o.x++;
    o.x++;

    assert.eq(1, t.find().count(), "Updates increased number of documents");
    assert.eq(o, t.findOne(), "Document doesn't match expected");
    assert.eq(true, res.results[0], "Bad result value for valid update");
    assert.eq(true, res.results[1], "Bad result value for valid update");

    //preCondition doesn't match ns
    res = db.runCommand({applyOps:
                            [
                                {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x: 1}}},
                                {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x: 1}}}
                            ],
                            preCondition: [{ns: "foo.otherName", q: {_id: 5}, res: {x: 21}}]
                        });

    assert.eq(o, t.findOne(), "preCondition didn't match, but ops were still applied");

    //preCondition doesn't match query
    res = db.runCommand({applyOps:
                            [
                                {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x : 1}}},
                                {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x : 1}}}
                            ],
                            preCondition: [{ns: t.getFullName(), q: {_id: 5}, res: {x: 19}}]
                        });

    assert.eq(o, t.findOne(), "preCondition didn't match, but ops were still applied");

    res = db.runCommand({applyOps:
                            [
                                {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x : 1}}},
                                {op: "u", ns: t.getFullName(), o2: {_id: 6}, o: {$inc: {x : 1}}}
                            ]
                        });

    assert.eq(true, res.results[0], "Valid update failed");
    assert.eq(true, res.results[1], "Valid update failed");
})();
