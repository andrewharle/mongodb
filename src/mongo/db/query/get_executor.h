
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

#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/parsed_distinct.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/update/update_driver.h"

namespace mongo {

class Collection;
class CountRequest;

struct GroupRequest;

/**
 * Filter indexes retrieved from index catalog by
 * allowed indices in query settings.
 * Used by getExecutor().
 * This function is public to facilitate testing.
 */
void filterAllowedIndexEntries(const AllowedIndicesFilter& allowedIndicesFilter,
                               std::vector<IndexEntry>* indexEntries);

/**
 * Fill out the provided 'plannerParams' for the 'canonicalQuery' operating on the collection
 * 'collection'.  Exposed for testing.
 */
void fillOutPlannerParams(OperationContext* opCtx,
                          Collection* collection,
                          CanonicalQuery* canonicalQuery,
                          QueryPlannerParams* plannerParams);

/**
 * Determines whether or not to wait for oplog visibility for a query. This is only used for
 * collection scans on the oplog.
 */
bool shouldWaitForOplogVisibility(OperationContext* opCtx,
                                  const Collection* collection,
                                  bool tailable);

/**
 * Get a plan executor for a query.
 *
 * If the query is valid and an executor could be created, returns a StatusWith with the
 * PlanExecutor.
 *
 * If the query cannot be executed, returns a Status indicating why.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutor(
    OperationContext* opCtx,
    Collection* collection,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    PlanExecutor::YieldPolicy yieldPolicy,
    size_t plannerOptions = 0);

/**
 * Get a plan executor for a .find() operation.
 *
 * If the query is valid and an executor could be created, returns a StatusWith with the
 * PlanExecutor.
 *
 * If the query cannot be executed, returns a Status indicating why.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorFind(
    OperationContext* opCtx,
    Collection* collection,
    const NamespaceString& nss,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    size_t plannerOptions = QueryPlannerParams::DEFAULT);

/**
 * Returns a plan executor for a legacy OP_QUERY find.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorLegacyFind(
    OperationContext* opCtx,
    Collection* collection,
    const NamespaceString& nss,
    std::unique_ptr<CanonicalQuery> canonicalQuery);

/**
 * If possible, turn the provided QuerySolution into a QuerySolution that uses a DistinctNode
 * to provide results for the distinct command.
 *
 * If the provided solution could be mutated successfully, returns true, otherwise returns
 * false.
 */
bool turnIxscanIntoDistinctIxscan(QuerySolution* soln, const std::string& field);

/*
 * Get an executor for a query executing as part of a distinct command.
 *
 * Distinct is unique in that it doesn't care about getting all the results; it just wants all
 * possible values of a certain field.  As such, we can skip lots of data in certain cases (see
 * body of method for detail).
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorDistinct(
    OperationContext* opCtx,
    Collection* collection,
    const std::string& ns,
    ParsedDistinct* parsedDistinct);

/*
 * Get a PlanExecutor for a query executing as part of a count command.
 *
 * Count doesn't care about actually examining its results; it just wants to walk through them.
 * As such, with certain covered queries, we can skip the overhead of fetching etc. when
 * executing a count.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorCount(
    OperationContext* opCtx, Collection* collection, const CountRequest& request, bool explain);

/**
 * Get a PlanExecutor for a delete operation. 'parsedDelete' describes the query predicate
 * and delete flags like 'isMulti'. The caller must hold the appropriate MODE_X or MODE_IX
 * locks, and must not release these locks until after the returned PlanExecutor is deleted.
 *
 * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
 *
 * The returned PlanExecutor will used the YieldPolicy returned by parsedDelete->yieldPolicy().
 *
 * Does not take ownership of its arguments.
 *
 * If the query is valid and an executor could be created, returns a StatusWith with the
 * PlanExecutor.
 *
 * If the query cannot be executed, returns a Status indicating why.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorDelete(
    OperationContext* opCtx, OpDebug* opDebug, Collection* collection, ParsedDelete* parsedDelete);

/**
 * Get a PlanExecutor for an update operation. 'parsedUpdate' describes the query predicate
 * and update modifiers. The caller must hold the appropriate MODE_X or MODE_IX locks prior
 * to calling this function, and must not release these locks until after the returned
 * PlanExecutor is deleted.
 *
 * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
 *
 * The returned PlanExecutor will used the YieldPolicy returned by parsedUpdate->yieldPolicy().
 *
 * Does not take ownership of its arguments.
 *
 * If the query is valid and an executor could be created, returns a StatusWith with the
 * PlanExecutor.
 *
 * If the query cannot be executed, returns a Status indicating why.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorUpdate(
    OperationContext* opCtx, OpDebug* opDebug, Collection* collection, ParsedUpdate* parsedUpdate);

/**
 * Get a PlanExecutor for a group operation.
 *
 * If the query is valid and an executor could be created, returns a StatusWith with the
 * PlanExecutor.
 *
 * If an executor could not be created, returns a Status indicating why.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> getExecutorGroup(
    OperationContext* opCtx, Collection* collection, const GroupRequest& request);

}  // namespace mongo
