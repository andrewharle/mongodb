// just make sure logout doesn't break anything
// @tags: [requires_auth]

// SERVER-724
db.runCommand({logout: 1});
x = db.runCommand({logout: 1});
assert.eq(1, x.ok, "A");

x = db.logout();
assert.eq(1, x.ok, "B");
