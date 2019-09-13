
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

#include <deque>

#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"

namespace mongo {

/**
 * Constructs and returns Documents from the BSONObj objects produced by a supplied PlanExecutor.
 */
class DocumentSourceCursor final : public DocumentSource {
public:
    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    BSONObjSet getOutputSorts() final {
        return _outputSorts;
    }
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed);

        constraints.requiresInputDocSource = false;
        return constraints;
    }

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    /**
     * Create a document source based on a passed-in PlanExecutor. 'exec' must be a yielding
     * PlanExecutor, and must be registered with the associated collection's CursorManager.
     */
    static boost::intrusive_ptr<DocumentSourceCursor> create(
        Collection* collection,
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        bool trackOplogTimestamp = false);

    /*
      Record the query that was specified for the cursor this wraps, if
      any.

      This should be captured after any optimizations are applied to
      the pipeline so that it reflects what is really used.

      This gets used for explain output.

      @param pBsonObj the query to record
     */
    void setQuery(const BSONObj& query) {
        _query = query;
    }

    /*
      Record the sort that was specified for the cursor this wraps, if
      any.

      This should be captured after any optimizations are applied to
      the pipeline so that it reflects what is really used.

      This gets used for explain output.

      @param pBsonObj the sort to record
     */
    void setSort(const BSONObj& sort) {
        _sort = sort;
    }

    /**
     * Informs this object of projection and dependency information.
     *
     * @param projection The projection that has been passed down to the query system.
     * @param deps The output of DepsTracker::toParsedDeps.
     */
    void setProjection(const BSONObj& projection, const boost::optional<ParsedDeps>& deps) {
        _projection = projection;
        _dependencies = deps;
    }

    /**
     * Returns the limit associated with this cursor, or -1 if there is no limit.
     */
    long long getLimit() const {
        return _limit ? _limit->getLimit() : -1;
    }

    /**
     * If subsequent sources need no information from the cursor, the cursor can simply output empty
     * documents, avoiding the overhead of converting BSONObjs to Documents.
     */
    void shouldProduceEmptyDocs() {
        _shouldProduceEmptyDocs = true;
    }

    Timestamp getLatestOplogTimestamp() const {
        return _latestOplogTimestamp;
    }

    const std::string& getPlanSummaryStr() const {
        return _planSummary;
    }

    const PlanSummaryStats& getPlanSummaryStats() const {
        return _planSummaryStats;
    }

protected:
    /**
     * Disposes of '_exec' if it hasn't been disposed already. This involves taking a collection
     * lock.
     */
    void doDispose() final;

    /**
     * Attempts to combine with any subsequent $limit stages by setting the internal '_limit' field.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
    DocumentSourceCursor(Collection* collection,
                         std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         bool trackOplogTimestamp = false);
    ~DocumentSourceCursor();

    /**
     * Acquires the appropriate locks, then destroys and de-registers '_exec'. '_exec' must be
     * non-null.
     */
    void cleanupExecutor();

    /**
     * Destroys and de-registers '_exec'. '_exec' must be non-null.
     */
    void cleanupExecutor(const AutoGetCollectionForRead& readLock);

    /**
     * Reads a batch of data from '_exec'.
     */
    void loadBatch();

    void recordPlanSummaryStats();

    /**
     * If we are tailing the oplog, this method updates the cached timestamp to that of the latest
     * document returned, or the latest timestamp observed in the oplog if we have no more results.
     */
    void _updateOplogTimestamp();

    // Batches results returned from the underlying PlanExecutor.
    std::deque<Document> _currentBatch;

    // BSONObj members must outlive _projection and cursor.
    BSONObj _query;
    BSONObj _sort;
    BSONObj _projection;
    bool _shouldProduceEmptyDocs = false;
    boost::optional<ParsedDeps> _dependencies;
    boost::intrusive_ptr<DocumentSourceLimit> _limit;
    long long _docsAddedToBatches;  // for _limit enforcement

    // The underlying query plan which feeds this pipeline. Must be destroyed while holding the
    // collection lock.
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _exec;

    // Status of the underlying executor, _exec. Used for explain queries if _exec produces an
    // error. Since _exec may not finish running (if there is a limit, for example), we store OK as
    // the default.
    Status _execStatus = Status::OK();

    BSONObjSet _outputSorts;
    std::string _planSummary;
    PlanSummaryStats _planSummaryStats;

    // Used only for explain() queries. Stores the stats of the winning plan when _exec's root
    // stage is a MultiPlanStage. When the query is executed (with exec->executePlan()), it will
    // wipe out its own copy of the winning plan's statistics, so they need to be saved here.
    std::unique_ptr<PlanStageStats> _winningPlanTrialStats;

    // True if we are tracking the latest observed oplog timestamp, false otherwise.
    bool _trackOplogTS = false;

    // If we are tailing the oplog and tracking the latest observed oplog time, this is the latest
    // timestamp seen in the collection. Otherwise, this is a null timestamp.
    Timestamp _latestOplogTimestamp;
};

}  // namespace mongo
