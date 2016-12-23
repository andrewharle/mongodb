/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lookup_set_cache.h"
#include "mongo/db/pipeline/value_comparator.h"

namespace mongo {

class DocumentSourceGraphLookUp final : public DocumentSourceNeedsMongod {
public:
    static std::unique_ptr<LiteParsedDocumentSourceOneForeignCollection> liteParse(
        const AggregationRequest& request, const BSONElement& spec);

    GetNextResult getNext() final;
    const char* getSourceName() const final;
    void dispose() final;
    BSONObjSet getOutputSorts() final;
    void serializeToArray(std::vector<Value>& array, bool explain = false) const final;

    /**
     * Returns the 'as' path, and possibly the fields modified by an absorbed $unwind.
     */
    GetModPathsReturn getModifiedPaths() const final;

    bool canSwapWithMatch() const final {
        return true;
    }

    /**
     * Attempts to combine with a subsequent $unwind stage, setting the internal '_unwind' field.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        _startWith->addDependencies(deps);
        return SEE_NEXT;
    };

    bool needsPrimaryShard() const final {
        return true;
    }

    void addInvolvedCollections(std::vector<NamespaceString>* collections) const final {
        collections->push_back(_from);
    }

    void doDetachFromOperationContext() final;

    void doReattachToOperationContext(OperationContext* opCtx) final;

    static boost::intrusive_ptr<DocumentSourceGraphLookUp> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        NamespaceString fromNs,
        std::string asField,
        std::string connectFromField,
        std::string connectToField,
        boost::intrusive_ptr<Expression> startWith,
        boost::optional<BSONObj> additionalFilter,
        boost::optional<FieldPath> depthField,
        boost::optional<long long> maxDepth,
        boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwindSrc);

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

protected:
    void doInjectExpressionContext() final;

private:
    DocumentSourceGraphLookUp(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        NamespaceString from,
        std::string as,
        std::string connectFromField,
        std::string connectToField,
        boost::intrusive_ptr<Expression> startWith,
        boost::optional<BSONObj> additionalFilter,
        boost::optional<FieldPath> depthField,
        boost::optional<long long> maxDepth,
        boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwindSrc);

    Value serialize(bool explain = false) const final {
        // Should not be called; use serializeToArray instead.
        MONGO_UNREACHABLE;
    }

    /**
     * Prepares the query to execute on the 'from' collection wrapped in a $match by using the
     * contents of '_frontier'.
     *
     * Fills 'cached' with any values that were retrieved from the cache.
     *
     * Returns boost::none if no query is necessary, i.e., all values were retrieved from the cache.
     * Otherwise, returns a query object.
     */
    boost::optional<BSONObj> makeMatchStageFromFrontier(BSONObjSet* cached);

    /**
     * If we have internalized a $unwind, getNext() dispatches to this function.
     */
    GetNextResult getNextUnwound();

    /**
     * Perform a breadth-first search of the 'from' collection. '_frontier' should already be
     * populated with the values for the initial query. Populates '_discovered' with the result(s)
     * of the query.
     */
    void doBreadthFirstSearch();

    /**
     * Populates '_frontier' with the '_startWith' value(s) from '_input' and then performs a
     * breadth-first search. Caller should check that _input is not boost::none.
     */
    void performSearch();

    /**
     * Updates '_cache' with 'result' appropriately, given that 'result' was retrieved when querying
     * for 'queried'.
     */
    void addToCache(const BSONObj& result, const ValueUnorderedSet& queried);

    /**
     * Assert that '_visited' and '_frontier' have not exceeded the maximum meory usage, and then
     * evict from '_cache' until this source is using less than '_maxMemoryUsageBytes'.
     */
    void checkMemoryUsage();

    /**
     * Process 'result', adding it to '_visited' with the given 'depth', and updating '_frontier'
     * with the object's 'connectTo' values.
     *
     * Returns whether '_visited' was updated, and thus, whether the search should recurse.
     */
    bool addToVisitedAndFrontier(BSONObj result, long long depth);

    // $graphLookup options.
    NamespaceString _from;
    FieldPath _as;
    FieldPath _connectFromField;
    FieldPath _connectToField;
    boost::intrusive_ptr<Expression> _startWith;
    boost::optional<BSONObj> _additionalFilter;
    boost::optional<FieldPath> _depthField;
    boost::optional<long long> _maxDepth;

    // The ExpressionContext used when performing aggregation pipelines against the '_from'
    // namespace.
    boost::intrusive_ptr<ExpressionContext> _fromExpCtx;

    // The aggregation pipeline to perform against the '_from' namespace.
    std::vector<BSONObj> _fromPipeline;

    size_t _maxMemoryUsageBytes = 100 * 1024 * 1024;

    // Track memory usage to ensure we don't exceed '_maxMemoryUsageBytes'.
    size_t _visitedUsageBytes = 0;
    size_t _frontierUsageBytes = 0;

    // Only used during the breadth-first search, tracks the set of values on the current frontier.
    // We use boost::optional to defer initialization until the ExpressionContext containing the
    // correct comparator is injected.
    boost::optional<ValueUnorderedSet> _frontier;

    // Tracks nodes that have been discovered for a given input. Keys are the '_id' value of the
    // document from the foreign collection, value is the document itself.  The keys are compared
    // using the simple collation.
    ValueUnorderedMap<BSONObj> _visited;

    // Caches query results to avoid repeating any work. This structure is maintained across calls
    // to getNext().
    LookupSetCache _cache;

    // When we have internalized a $unwind, we must keep track of the input document, since we will
    // need it for multiple "getNext()" calls.
    boost::optional<Document> _input;

    // The variables that are in scope to be used by the '_startWith' expression.
    std::unique_ptr<Variables> _variables;

    // Keep track of a $unwind that was absorbed into this stage.
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> _unwind;

    // If we absorbed a $unwind that specified 'includeArrayIndex', this is used to populate that
    // field, tracking how many results we've returned so far for the current input document.
    long long _outputIndex;
};

}  // namespace mongo
