
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_cursor.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;

const char* DocumentSourceCursor::getSourceName() const {
    return "$cursor";
}

bool DocumentSourceCursor::Batch::isEmpty() const {
    if (shouldProduceEmptyDocs) {
        return !_count;
    } else {
        return _batchOfDocs.empty();
    }
    MONGO_UNREACHABLE;
}

void DocumentSourceCursor::Batch::enqueue(Document&& doc) {
    if (shouldProduceEmptyDocs) {
        ++_count;
    } else {
        _batchOfDocs.push_back(doc.getOwned());
        _memUsageBytes += _batchOfDocs.back().getApproximateSize();
    }
}

Document DocumentSourceCursor::Batch::dequeue() {
    invariant(!isEmpty());
    if (shouldProduceEmptyDocs) {
        --_count;
        return Document{};
    } else {
        Document out = std::move(_batchOfDocs.front());
        _batchOfDocs.pop_front();
        if (_batchOfDocs.empty()) {
            _memUsageBytes = 0;
        }
        return out;
    }
    MONGO_UNREACHABLE;
}

void DocumentSourceCursor::Batch::clear() {
    _batchOfDocs.clear();
    _count = 0;
    _memUsageBytes = 0;
}

DocumentSource::GetNextResult DocumentSourceCursor::getNext() {
    pExpCtx->checkForInterrupt();

    if (_currentBatch.isEmpty()) {
        loadBatch();
    }

    // If we are tracking the oplog timestamp, update our cached latest optime.
    if (_trackOplogTS && _exec)
        _updateOplogTimestamp();

    if (_currentBatch.isEmpty())
        return GetNextResult::makeEOF();

    return _currentBatch.dequeue();
}

void DocumentSourceCursor::loadBatch() {
    if (!_exec || _exec->isDisposed()) {
        // No more documents.
        return;
    }

    PlanExecutor::ExecState state;
    BSONObj resultObj;
    {
        AutoGetCollectionForRead autoColl(pExpCtx->opCtx, _exec->nss());
        uassertStatusOK(repl::ReplicationCoordinator::get(pExpCtx->opCtx)
                            ->checkCanServeReadsFor(pExpCtx->opCtx, _exec->nss(), true));

        uassertStatusOK(_exec->restoreState());

        {
            ON_BLOCK_EXIT([this] { recordPlanSummaryStats(); });

            while ((state = _exec->getNext(&resultObj, nullptr)) == PlanExecutor::ADVANCED) {
                if (_currentBatch.shouldProduceEmptyDocs) {
                    _currentBatch.enqueue(Document());
                } else if (_dependencies) {
                    _currentBatch.enqueue(_dependencies->extractFields(resultObj));
                } else {
                    _currentBatch.enqueue(Document::fromBsonWithMetaData(resultObj));
                }

                if (_limit) {
                    if (++_docsAddedToBatches == _limit->getLimit()) {
                        break;
                    }
                    verify(_docsAddedToBatches < _limit->getLimit());
                }

                // As long as we're waiting for inserts, we shouldn't do any batching at this level
                // we need the whole pipeline to see each document to see if we should stop waiting.
                if (awaitDataState(pExpCtx->opCtx).shouldWaitForInserts ||
                    static_cast<long long>(_currentBatch.memUsageBytes()) >
                        internalDocumentSourceCursorBatchSizeBytes.load()) {
                    // End this batch and prepare PlanExecutor for yielding.
                    _exec->saveState();
                    return;
                }
            }
            // Special case for tailable cursor -- EOF doesn't preclude more results, so keep
            // the PlanExecutor alive.
            if (state == PlanExecutor::IS_EOF && pExpCtx->isTailableAwaitData()) {
                _exec->saveState();
                return;
            }
        }

        // If we got here, there won't be any more documents, so destroy our PlanExecutor. Note we
        // must hold a collection lock to destroy '_exec', but we can only assume that our locks are
        // still held if '_exec' did not end in an error. If '_exec' encountered an error during a
        // yield, the locks might be yielded.
        if (state != PlanExecutor::DEAD && state != PlanExecutor::FAILURE) {
            cleanupExecutor(autoColl);
        }
    }

    switch (state) {
        case PlanExecutor::ADVANCED:
        case PlanExecutor::IS_EOF:
            return;  // We've reached our limit or exhausted the cursor.
        case PlanExecutor::DEAD:
        case PlanExecutor::FAILURE: {
            _execStatus = WorkingSetCommon::getMemberObjectStatus(resultObj).withContext(
                "Error in $cursor stage");
            uassertStatusOK(_execStatus);
        }
        default:
            MONGO_UNREACHABLE;
    }
}

void DocumentSourceCursor::_updateOplogTimestamp() {
    // If we are about to return a result, set our oplog timestamp to the optime of that result.
    if (!_currentBatch.isEmpty()) {
        const auto& ts = _currentBatch.peekFront().getField(repl::OpTime::kTimestampFieldName);
        invariant(ts.getType() == BSONType::bsonTimestamp);
        _latestOplogTimestamp = ts.getTimestamp();
        return;
    }

    // If we have no more results to return, advance to the latest oplog timestamp.
    _latestOplogTimestamp = _exec->getLatestOplogTimestamp();
}

Pipeline::SourceContainer::iterator DocumentSourceCursor::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextLimit = dynamic_cast<DocumentSourceLimit*>((*std::next(itr)).get());

    if (nextLimit) {
        if (_limit) {
            // We already have an internal limit, set it to the more restrictive of the two.
            _limit->setLimit(std::min(_limit->getLimit(), nextLimit->getLimit()));
        } else {
            _limit = nextLimit;
        }
        container->erase(std::next(itr));
        return itr;
    }
    return std::next(itr);
}

void DocumentSourceCursor::recordPlanSummaryStats() {
    invariant(_exec);
    // Aggregation handles in-memory sort outside of the query sub-system. Given that we need to
    // preserve the existing value of hasSortStage rather than overwrite with the underlying
    // PlanExecutor's value.
    auto hasSortStage = _planSummaryStats.hasSortStage;

    Explain::getSummaryStats(*_exec, &_planSummaryStats);

    _planSummaryStats.hasSortStage = hasSortStage;
}

Value DocumentSourceCursor::serialize(boost::optional<ExplainOptions::Verbosity> verbosity) const {
    // We never parse a DocumentSourceCursor, so we only serialize for explain.
    if (!verbosity)
        return Value();

    invariant(_exec);

    uassert(50660,
            "Mismatch between verbosity passed to serialize() and expression context verbosity",
            verbosity == pExpCtx->explain);

    MutableDocument out;
    out["query"] = Value(_query);

    if (!_sort.isEmpty())
        out["sort"] = Value(_sort);

    if (_limit)
        out["limit"] = Value(_limit->getLimit());

    if (!_projection.isEmpty())
        out["fields"] = Value(_projection);

    BSONObjBuilder explainStatsBuilder;

    {
        auto opCtx = pExpCtx->opCtx;
        auto lockMode = getLockModeForQuery(opCtx);
        AutoGetDb dbLock(opCtx, _exec->nss().db(), lockMode);
        Lock::CollectionLock collLock(opCtx->lockState(), _exec->nss().ns(), lockMode);
        auto collection =
            dbLock.getDb() ? dbLock.getDb()->getCollection(opCtx, _exec->nss()) : nullptr;

        Explain::explainStages(_exec.get(),
                               collection,
                               verbosity.get(),
                               _execStatus,
                               _winningPlanTrialStats.get(),
                               &explainStatsBuilder);
    }

    BSONObj explainStats = explainStatsBuilder.obj();
    invariant(explainStats["queryPlanner"]);
    out["queryPlanner"] = Value(explainStats["queryPlanner"]);

    if (verbosity.get() >= ExplainOptions::Verbosity::kExecStats) {
        invariant(explainStats["executionStats"]);
        out["executionStats"] = Value(explainStats["executionStats"]);
    }

    return Value(DOC(getSourceName() << out.freezeToValue()));
}

void DocumentSourceCursor::detachFromOperationContext() {
    if (_exec) {
        _exec->detachFromOperationContext();
    }
}

void DocumentSourceCursor::reattachToOperationContext(OperationContext* opCtx) {
    if (_exec) {
        _exec->reattachToOperationContext(opCtx);
    }
}

void DocumentSourceCursor::doDispose() {
    _currentBatch.clear();
    if (!_exec || _exec->isDisposed()) {
        // We've already properly disposed of our PlanExecutor.
        return;
    }
    cleanupExecutor();
}

void DocumentSourceCursor::cleanupExecutor() {
    invariant(_exec);
    auto* opCtx = pExpCtx->opCtx;
    // We need to be careful to not use AutoGetCollection here, since we only need the lock to
    // protect potential access to the Collection's CursorManager, and AutoGetCollection may throw
    // if this namespace has since turned into a view. Using Database::getCollection() will simply
    // return nullptr if the collection has since turned into a view. In this case, '_exec' will
    // already have been marked as killed when the collection was dropped, and we won't need to
    // access the CursorManager to properly dispose of it.
    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    auto lockMode = getLockModeForQuery(opCtx);
    AutoGetDb dbLock(opCtx, _exec->nss().db(), lockMode);
    Lock::CollectionLock collLock(opCtx->lockState(), _exec->nss().ns(), lockMode);
    auto collection = dbLock.getDb() ? dbLock.getDb()->getCollection(opCtx, _exec->nss()) : nullptr;
    auto cursorManager = collection ? collection->getCursorManager() : nullptr;
    _exec->dispose(opCtx, cursorManager);

    // Not freeing _exec if we're in explain mode since it will be used in serialize() to gather
    // execution stats.
    if (!pExpCtx->explain) {
        _exec.reset();
    }
}

void DocumentSourceCursor::cleanupExecutor(const AutoGetCollectionForRead& readLock) {
    invariant(_exec);
    auto cursorManager =
        readLock.getCollection() ? readLock.getCollection()->getCursorManager() : nullptr;
    _exec->dispose(pExpCtx->opCtx, cursorManager);

    // Not freeing _exec if we're in explain mode since it will be used in serialize() to gather
    // execution stats.
    if (!pExpCtx->explain) {
        _exec.reset();
    }
}

DocumentSourceCursor::~DocumentSourceCursor() {
    if (pExpCtx->explain) {
        invariant(_exec->isDisposed());  // _exec should have at least been disposed.
    } else {
        invariant(!_exec);  // '_exec' should have been cleaned up via dispose() before destruction.
    }
}

DocumentSourceCursor::DocumentSourceCursor(
    Collection* collection,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const intrusive_ptr<ExpressionContext>& pCtx,
    bool trackOplogTimestamp)
    : DocumentSource(pCtx),
      _docsAddedToBatches(0),
      _exec(std::move(exec)),
      _outputSorts(_exec->getOutputSorts()),
      _trackOplogTS(trackOplogTimestamp) {

    _planSummary = Explain::getPlanSummary(_exec.get());
    recordPlanSummaryStats();

    if (pExpCtx->explain) {
        // It's safe to access the executor even if we don't have the collection lock since we're
        // just going to call getStats() on it.
        _winningPlanTrialStats = Explain::getWinningPlanTrialStats(_exec.get());
    }

    if (collection) {
        collection->infoCache()->notifyOfQuery(pExpCtx->opCtx, _planSummaryStats.indexesUsed);
    }
}

intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
    Collection* collection,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    bool trackOplogTimestamp) {
    intrusive_ptr<DocumentSourceCursor> source(
        new DocumentSourceCursor(collection, std::move(exec), pExpCtx, trackOplogTimestamp));
    return source;
}
}  // namespace mongo
