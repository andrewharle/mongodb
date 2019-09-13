/**
 * Enables causal consistency on the connections.
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");
    load('jstests/libs/override_methods/set_read_preference_secondary.js');

    db.getMongo().setCausalConsistency();

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/enable_causal_consistency.js");
})();
