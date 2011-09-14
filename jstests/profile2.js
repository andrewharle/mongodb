print("profile2.js BEGIN");

try {

    assert.commandWorked( db.runCommand( {profile:2} ) );

    huge = 'huge';
    while (huge.length < 2*1024*1024){
        huge += huge;
    }

    db.profile2.count({huge:huge}) // would make a huge entry in db.system.profile

    print("profile2.js SUCCESS OK");
    
} finally {
    // disable profiling for subsequent tests
    assert.commandWorked( db.runCommand( {profile:0} ) );
}
