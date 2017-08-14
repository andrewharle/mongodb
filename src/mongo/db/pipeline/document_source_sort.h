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
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/sorter/sorter.h"

namespace mongo {

class Expression;

class DocumentSourceSort final : public DocumentSource, public SplittableDocumentSource {
public:
    static const uint64_t kMaxMemoryUsageBytes = 100 * 1024 * 1024;

    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    void serializeToArray(std::vector<Value>& array, bool explain = false) const final;

    GetModPathsReturn getModifiedPaths() const final {
        // A $sort does not modify any paths.
        return {GetModPathsReturn::Type::kFiniteSet, std::set<std::string>{}};
    }

    bool canSwapWithMatch() const final {
        // Can't swap with a $match if a limit has been absorbed, since in general match can't swap
        // with limit.
        return !limitSrc;
    }

    BSONObjSet getOutputSorts() final {
        return allPrefixes(_sort);
    }

    /**
     * Attempts to absorb a subsequent $limit stage so that it an perform a top-k sort.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;
    void dispose() final;

    GetDepsReturn getDependencies(DepsTracker* deps) const final;

    boost::intrusive_ptr<DocumentSource> getShardSource() final;
    boost::intrusive_ptr<DocumentSource> getMergeSource() final;

    /// Write out a Document whose contents are the sort key.
    Document serializeSortKey(bool explain) const;

    /**
     * Parses a $sort stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Convenience method for creating a $sort stage.
     */
    static boost::intrusive_ptr<DocumentSourceSort> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        BSONObj sortOrder,
        long long limit = -1,
        uint64_t maxMemoryUsageBytes = kMaxMemoryUsageBytes);

    /**
     * Returns -1 for no limit.
     */
    long long getLimit() const;

    /**
     * Loads a document to be sorted. This can be used to sort a stream of documents that are not
     * coming from another DocumentSource. Once all documents have been added, the caller must call
     * loadingDone() before using getNext() to receive the documents in sorted order.
     */
    void loadDocument(const Document& doc);

    /**
     * Signals to the sort stage that there will be no more input documents. It is an error to call
     * loadDocument() once this method returns.
     */
    void loadingDone();

    /**
     * Instructs the sort stage to use the given set of cursors as inputs, to merge documents that
     * have already been sorted.
     */
    void populateFromCursors(const std::vector<DBClientCursor*>& cursors);

    bool isPopulated() {
        return _populated;
    };

    boost::intrusive_ptr<DocumentSourceLimit> getLimitSrc() const {
        return limitSrc;
    }

private:
    explicit DocumentSourceSort(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    Value serialize(bool explain = false) const final {
        MONGO_UNREACHABLE;  // Should call serializeToArray instead.
    }

    /**
     * Helper to add a sort key to this stage.
     */
    void addKey(StringData fieldPath, bool ascending);

    /**
     * Before returning anything, we have to consume all input and sort it. This method consumes all
     * input and prepares the sorted stream '_output'.
     *
     * This method may not be able to finish populating the sorter in a single call if 'pSource'
     * returns a DocumentSource::GetNextResult::kPauseExecution, so it returns the last
     * GetNextResult encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult populate();
    bool _populated = false;

    BSONObj _sort;

    SortOptions makeSortOptions() const;

    // This is used to merge pre-sorted results from a DocumentSourceMergeCursors.
    class IteratorFromCursor;

    /* these two parallel each other */
    typedef std::vector<boost::intrusive_ptr<Expression>> SortKey;
    SortKey vSortKey;
    std::vector<char> vAscending;  // used like std::vector<bool> but without specialization

    /// Extracts the fields in vSortKey from the Document;
    Value extractKey(const Document& d) const;

    /// Compare two Values according to the specified sort key.
    int compare(const Value& lhs, const Value& rhs) const;

    typedef Sorter<Value, Document> MySorter;

    /**
     * Absorbs 'limit', enabling a top-k sort. It is safe to call this multiple times, it will keep
     * the smallest limit.
     */
    void setLimitSrc(boost::intrusive_ptr<DocumentSourceLimit> limit) {
        if (!limitSrc || limit->getLimit() < limitSrc->getLimit()) {
            limitSrc = limit;
        }
    }

    // For MySorter
    class Comparator {
    public:
        explicit Comparator(const DocumentSourceSort& source) : _source(source) {}
        int operator()(const MySorter::Data& lhs, const MySorter::Data& rhs) const {
            return _source.compare(lhs.first, rhs.first);
        }

    private:
        const DocumentSourceSort& _source;
    };

    boost::intrusive_ptr<DocumentSourceLimit> limitSrc;

    uint64_t _maxMemoryUsageBytes;
    bool _done;
    bool _mergingPresorted;
    std::unique_ptr<MySorter> _sorter;
    std::unique_ptr<MySorter::Iterator> _output;
};

}  // namespace mongo
