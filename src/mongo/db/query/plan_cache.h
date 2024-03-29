
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional/optional.hpp>
#include <set>

#include "mongo/base/counter.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/lru_key_value.h"
#include "mongo/db/query/plan_cache_indexability.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/container_size_helper.h"

namespace mongo {

// A PlanCacheKey is a string-ified version of a query's predicate/projection/sort.
typedef std::string PlanCacheKey;

struct QuerySolution;
struct QuerySolutionNode;

/**
 * When the CachedPlanStage runs a cached query, it can provide feedback to the cache.  This
 * feedback is available to anyone who retrieves that query in the future.
 */
struct PlanCacheEntryFeedback {
    uint64_t estimateObjectSizeInBytes() const {
        return stats->estimateObjectSizeInBytes() + sizeof(*this);
    }

    std::unique_ptr<PlanCacheEntryFeedback> clone() const {
        auto clonedFeedback = stdx::make_unique<PlanCacheEntryFeedback>();
        clonedFeedback->stats.reset(stats->clone());
        clonedFeedback->score = score;
        return clonedFeedback;
    }

    // How well did the cached plan perform?
    std::unique_ptr<PlanStageStats> stats;

    // The "goodness" score produced by the plan ranker
    // corresponding to 'stats'.
    double score;
};

// TODO: Replace with opaque type.
typedef std::string PlanID;

/**
 * A PlanCacheIndexTree is the meaty component of the data
 * stored in SolutionCacheData. It is a tree structure with
 * index tags that indicates to the access planner which indices
 * it should try to use.
 *
 * How a PlanCacheIndexTree is created:
 *   The query planner tags a match expression with indices. It
 *   then uses the tagged tree to create a PlanCacheIndexTree,
 *   using QueryPlanner::cacheDataFromTaggedTree. The PlanCacheIndexTree
 *   is isomorphic to the tagged match expression, and has matching
 *   index tags.
 *
 * How a PlanCacheIndexTree is used:
 *   When the query planner is planning from the cache, it uses
 *   the PlanCacheIndexTree retrieved from the cache in order to
 *   recreate index assignments. Specifically, a raw MatchExpression
 *   is tagged according to the index tags in the PlanCacheIndexTree.
 *   This is done by QueryPlanner::tagAccordingToCache.
 */
struct PlanCacheIndexTree {

    /**
     * An OrPushdown is the cached version of an OrPushdownTag::Destination. It indicates that this
     * node is a predicate that can be used inside of a sibling indexed OR, to tighten index bounds
     * or satisfy the first field in the index.
     */
    struct OrPushdown {
        std::string indexName;
        uint64_t estimateObjectSizeInBytes() const {
            return  // Add size of each element in 'route' vector.
                container_size_helper::estimateObjectSizeInBytes(route) +
                // Add size of each element in 'route' vector.
                indexName.size() +
                // Add size of the object.
                sizeof(*this);
        }
        size_t position;
        bool canCombineBounds;
        std::deque<size_t> route;
    };

    PlanCacheIndexTree() : entry(nullptr), index_pos(0), canCombineBounds(true) {}

    ~PlanCacheIndexTree() {
        for (std::vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
             it != children.end();
             ++it) {
            delete *it;
        }
    }

    /**
     * Clone 'ie' and set 'this->entry' to be the clone.
     */
    void setIndexEntry(const IndexEntry& ie);

    /**
     * Make a deep copy.
     */
    PlanCacheIndexTree* clone() const;

    /**
     * For debugging.
     */
    std::string toString(int indents = 0) const;

    uint64_t estimateObjectSizeInBytes() const {
        return  // Recursively add size of each element in 'children' vector.
            container_size_helper::estimateObjectSizeInBytes(
                children,
                [](const auto& child) { return child->estimateObjectSizeInBytes(); },
                true) +
            // Add size of each element in 'orPushdowns' vector.
            container_size_helper::estimateObjectSizeInBytes(
                orPushdowns,
                [](const auto& orPushdown) { return orPushdown.estimateObjectSizeInBytes(); },
                false) +
            // Add size of 'entry' if present.
            (entry ? entry->estimateObjectSizeInBytes() : 0) +
            // Add size of the object.
            sizeof(*this);
    }
    // Children owned here.
    std::vector<PlanCacheIndexTree*> children;

    // Owned here.
    std::unique_ptr<IndexEntry> entry;

    size_t index_pos;

    // The value for this member is taken from the IndexTag of the corresponding match expression
    // and is used to ensure that bounds are correctly intersected and/or compounded when a query is
    // planned from the plan cache.
    bool canCombineBounds;

    std::vector<OrPushdown> orPushdowns;
};

/**
 * Data stored inside a QuerySolution which can subsequently be
 * used to create a cache entry. When this data is retrieved
 * from the cache, it is sufficient to reconstruct the original
 * QuerySolution.
 */
struct SolutionCacheData {
    SolutionCacheData()
        : tree(nullptr),
          solnType(USE_INDEX_TAGS_SOLN),
          wholeIXSolnDir(1),
          indexFilterApplied(false) {}

    std::unique_ptr<SolutionCacheData> clone() const;

    // For debugging.
    std::string toString() const;

    uint64_t estimateObjectSizeInBytes() const {
        return (tree ? tree->estimateObjectSizeInBytes() : 0) + sizeof(*this);
    }

    // Owned here. If 'wholeIXSoln' is false, then 'tree'
    // can be used to tag an isomorphic match expression. If 'wholeIXSoln'
    // is true, then 'tree' is used to store the relevant IndexEntry.
    // If 'collscanSoln' is true, then 'tree' should be NULL.
    std::unique_ptr<PlanCacheIndexTree> tree;

    enum SolutionType {
        // Indicates that the plan should use
        // the index as a proxy for a collection
        // scan (e.g. using index to provide sort).
        WHOLE_IXSCAN_SOLN,

        // The cached plan is a collection scan.
        COLLSCAN_SOLN,

        // Build the solution by using 'tree'
        // to tag the match expression.
        USE_INDEX_TAGS_SOLN
    } solnType;

    // The direction of the index scan used as
    // a proxy for a collection scan. Used only
    // for WHOLE_IXSCAN_SOLN.
    int wholeIXSolnDir;

    // True if index filter was applied.
    bool indexFilterApplied;
};

class PlanCacheEntry;

/**
 * Information returned from a get(...) query.
 */
class CachedSolution {
private:
    MONGO_DISALLOW_COPYING(CachedSolution);

public:
    CachedSolution(const PlanCacheEntry& entry);

    // Information that can be used by the QueryPlanner to reconstitute the complete execution plan.
    std::unique_ptr<SolutionCacheData> plannerData;

    // The number of work cycles taken to decide on a winning plan when the plan was first cached.
    size_t decisionWorks;
};

/**
 * Used by the cache to track entries and their performance over time.
 * Also used by the plan cache commands to display plan cache state.
 */
class PlanCacheEntry {
private:
    MONGO_DISALLOW_COPYING(PlanCacheEntry);

public:
    /**
     * A description of the query from which a 'PlanCacheEntry' was created.
     */
    struct CreatedFromQuery {
        /**
         * Returns an estimate of the size of this object, including the memory allocated elsewhere
         * that it owns, in bytes.
         */
        uint64_t estimateObjectSizeInBytes() const;

        std::string debugString() const;

        BSONObj filter;
        BSONObj sort;
        BSONObj projection;
        BSONObj collation;
    };

    /**
     * Per-plan cache entry information that is used strictly as debug information (e.g. is intended
     * for display by the 'planCacheListPlans' command). In order to save memory, this information
     * is sometimes discarded instead of kept in the plan cache entry. Therefore, this information
     * may not be used for any purpose outside displaying debug info, such as recovering a plan from
     * the cache or determining whether or not the cache entry is active.
     */
    struct DebugInfo {
        DebugInfo(CreatedFromQuery createdFromQuery,
                  std::unique_ptr<const PlanRankingDecision> decision,
                  std::vector<std::unique_ptr<PlanCacheEntryFeedback>> feedback);

        /**
         * Returns an estimate of the size of this object, including the memory allocated elsewhere
         * that it owns, in bytes.
         */
        uint64_t estimateObjectSizeInBytes() const;

        std::unique_ptr<DebugInfo> clone() const;

        CreatedFromQuery createdFromQuery;

        // Information that went into picking the winning plan and also why the other plans lost.
        // Never nullptr.
        std::unique_ptr<const PlanRankingDecision> decision;

        // Scores from uses of this cache entry.
        std::vector<std::unique_ptr<PlanCacheEntryFeedback>> feedback;
    };

    /**
     * Create a new PlanCacheEntry.
     * Grabs any planner-specific data required from the solutions.
     */
    static std::unique_ptr<PlanCacheEntry> create(
        const std::vector<QuerySolution*>& solutions,
        std::unique_ptr<const PlanRankingDecision> decision,
        const CanonicalQuery& query,
        Date_t timeOfCreation);

    ~PlanCacheEntry();

    /**
     * Make a deep copy.
     */
    PlanCacheEntry* clone() const;

    std::string debugString() const;

    // Data provided to the planner to allow it to recreate the solution this entry represents. In
    // order to return it from the cache for consumption by the 'QueryPlanner', a deep copy is made
    // and returned inside 'CachedSolution'.
    //
    // The first element of the vector is the cache data associated with the winning plan. The
    // remaining elements correspond to the rejected plans, sorted by descending score.
    const std::vector<std::unique_ptr<const SolutionCacheData>> plannerData;

    const Date_t timeOfCreation;

    // The number of work taken to select the winning plan when this plan cache entry was first
    // created.
    const size_t decisionWorks;

    // Optional debug info containing detailed statistics. Includes a description of the query which
    // resulted in this plan cache's creation as well as runtime stats from the multi-planner trial
    // period that resulted in this cache entry.
    //
    // Once the estimated cumulative size of the mongod's plan caches exceeds a threshold, this
    // debug info is omitted from new plan cache entries.
    std::unique_ptr<DebugInfo> debugInfo;

    // An estimate of the size in bytes of this plan cache entry. This is the "deep size",
    // calculated by recursively incorporating the size of owned objects, the objects that they in
    // turn own, and so on.
    const uint64_t estimatedEntrySizeBytes;

    /**
     * Tracks the approximate cumulative size of the plan cache entries across all the collections.
     */
    static Counter64 planCacheTotalSizeEstimateBytes;

private:
    /**
     * All arguments constructor.
     */
    PlanCacheEntry(std::vector<std::unique_ptr<const SolutionCacheData>> plannerData,
                   Date_t timeOfCreation,
                   size_t decisionWorks,
                   std::unique_ptr<DebugInfo> debugInfo);

    uint64_t _estimateObjectSizeInBytes() const;
};

/**
 * Caches the best solution to a query.  Aside from the (CanonicalQuery -> QuerySolution)
 * mapping, the cache contains information on why that mapping was made and statistics on the
 * cache entry's actual performance on subsequent runs.
 *
 */
class PlanCache {
private:
    MONGO_DISALLOW_COPYING(PlanCache);

public:
    /**
     * We don't want to cache every possible query. This function
     * encapsulates the criteria for what makes a canonical query
     * suitable for lookup/inclusion in the cache.
     */
    static bool shouldCacheQuery(const CanonicalQuery& query);

    /**
     * If omitted, namespace set to empty string.
     */
    PlanCache();

    PlanCache(const std::string& ns);

    ~PlanCache();

    /**
     * Record solutions for query. Best plan is first element in list.
     * Each query in the cache will have more than 1 plan because we only
     * add queries which are considered by the multi plan runner (which happens
     * only when the query planner generates multiple candidate plans). Callers are responsible
     * for passing the current time so that the time the plan cache entry was created is stored
     * in the plan cache.
     *
     * Takes ownership of 'why'.
     *
     * If the mapping was added successfully, returns Status::OK().
     * If the mapping already existed or some other error occurred, returns another Status.
     */
    Status add(const CanonicalQuery& query,
               const std::vector<QuerySolution*>& solns,
               std::unique_ptr<PlanRankingDecision> why,
               Date_t now);

    /**
     * Look up the cached data access for the provided 'query'.  Used by the query planner
     * to shortcut planning.
     *
     * If there is no entry in the cache for the 'query', returns an error Status.
     *
     * If there is an entry in the cache, populates 'crOut' and returns Status::OK().  Caller
     * owns '*crOut'.
     */
    Status get(const CanonicalQuery& query, CachedSolution** crOut) const;

    /**
     * When the CachedPlanStage runs a plan out of the cache, we want to record data about the
     * plan's performance.  The CachedPlanStage calls feedback(...) after executing the cached
     * plan for a trial period in order to do this.
     *
     * Cache takes ownership of 'feedback'.
     *
     * If the entry corresponding to 'cq' isn't in the cache anymore, the feedback is ignored
     * and an error Status is returned.
     *
     * If the entry corresponding to 'cq' still exists, 'feedback' is added to the run
     * statistics about the plan.  Status::OK() is returned.
     */
    Status feedback(const CanonicalQuery& cq, PlanCacheEntryFeedback* feedback);

    /**
     * Remove the entry corresponding to 'ck' from the cache.  Returns Status::OK() if the plan
     * was present and removed and an error status otherwise.
     */
    Status remove(const CanonicalQuery& canonicalQuery);

    /**
     * Remove *all* cached plans.  Does not clear index information.
     */
    void clear();

    /**
     * Get the cache key corresponding to the given canonical query.  The query need not already
     * be cached.
     *
     * This is provided in the public API simply as a convenience for consumers who need some
     * description of query shape (e.g. index filters).
     *
     * Callers must hold the collection lock when calling this method.
     */
    PlanCacheKey computeKey(const CanonicalQuery&) const;

    /**
     * Returns a copy of a cache entry.
     * Used by planCacheListPlans to display plan details.
     *
     * If there is no entry in the cache for the 'query', returns an error Status.
     *
     * If there is an entry in the cache, populates 'entryOut' and returns Status::OK().  Caller
     * owns '*entryOut'.
     */
    Status getEntry(const CanonicalQuery& cq, PlanCacheEntry** entryOut) const;

    /**
     * Returns a vector of all cache entries.
     * Caller owns the result vector and is responsible for cleaning up
     * the cache entry copies.
     * Used by planCacheListQueryShapes and index_filter_commands_test.cpp.
     */
    std::vector<PlanCacheEntry*> getAllEntries() const;

    /**
     * Returns true if there is an entry in the cache for the 'query'.
     * Internally calls hasKey() on the LRU cache.
     */
    bool contains(const CanonicalQuery& cq) const;

    /**
     * Returns number of entries in cache.
     * Used for testing.
     */
    size_t size() const;

    /**
     * Updates internal state kept about the collection's indexes.  Must be called when the set
     * of indexes on the associated collection have changed.
     *
     * Callers must hold the collection lock in exclusive mode when calling this method.
     */
    void notifyOfIndexEntries(const std::vector<IndexEntry>& indexEntries);

private:
    void encodeKeyForMatch(const MatchExpression* tree, StringBuilder* keyBuilder) const;
    void encodeKeyForSort(const BSONObj& sortObj, StringBuilder* keyBuilder) const;
    void encodeKeyForProj(const BSONObj& projObj, StringBuilder* keyBuilder) const;

    LRUKeyValue<PlanCacheKey, PlanCacheEntry> _cache;

    // Protects _cache.
    mutable stdx::mutex _cacheMutex;

    // Full namespace of collection.
    std::string _ns;

    // Holds computed information about the collection's indexes.  Used for generating plan
    // cache keys.
    //
    // Concurrent access is synchronized by the collection lock.  Multiple concurrent readers
    // are allowed.
    PlanCacheIndexabilityState _indexabilityState;
};

}  // namespace mongo
