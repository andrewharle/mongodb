// Contains helpers for checking, based on the explain output, properties of a
// plan. For instance, there are helpers for checking whether a plan is a collection
// scan or whether the plan is covered (index only).

/**
 * Given the root stage of explain's JSON representation of a query plan ('root'), returns the
 * subdocument with its stage as 'stage'. Returns null if the plan does not have such a stage.
 */
function getPlanStage(root, stage) {
    if (root.stage === stage) {
        return root;
    } else if ("inputStage" in root) {
        return getPlanStage(root.inputStage, stage);
    } else if ("inputStages" in root) {
        for (var i = 0; i < root.inputStages.length; i++) {
            var stage = getPlanStage(root.inputStages[i], stage);
            if (stage !== null) {
                return stage;
            }
        }
    } else if ("shards" in root) {
        for (var i = 0; i < root.shards.length; i++) {
            var stage = getPlanStage(root.shards[i].winningPlan, stage);
            if (stage !== null) {
                return stage;
            }
        }
    }

    return null;
}

/**
 * Given the root stage of explain's BSON representation of a query plan ('root'),
 * returns true if the plan has a stage called 'stage'.
 */
function planHasStage(root, stage) {
    return getPlanStage(root, stage) !== null;
}

/**
 * A query is covered iff it does *not* have a FETCH stage or a COLLSCAN.
 *
 * Given the root stage of explain's BSON representation of a query plan ('root'),
 * returns true if the plan is index only. Otherwise returns false.
 */
function isIndexOnly(root) {
    return !planHasStage(root, "FETCH") && !planHasStage(root, "COLLSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * an index scan, and false otherwise.
 */
function isIxscan(root) {
    return planHasStage(root, "IXSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * the idhack fast path, and false otherwise.
 */
function isIdhack(root) {
    return planHasStage(root, "IDHACK");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * a collection scan, and false otherwise.
 */
function isCollscan(root) {
    return planHasStage(root, "COLLSCAN");
}

/**
 * Get the number of chunk skips for the BSON exec stats tree rooted at 'root'.
 */
function getChunkSkips(root) {
    if (root.stage === "SHARDING_FILTER") {
        return root.chunkSkips;
    }
    else if ("inputStage" in root) {
        return getChunkSkips(root.inputStage);
    }
    else if ("inputStages" in root) {
        var skips = 0;
        for (var i = 0; i < root.inputStages.length; i++) {
            skips += getChunkSkips(root.inputStages[0]);
        }
        return skips;
    }

    return 0;
}
