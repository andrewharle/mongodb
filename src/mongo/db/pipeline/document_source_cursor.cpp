/**
 * Copyright 2011 (c) 10gen Inc.
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
 */

#include "mongo/pch.h"

#include "mongo/db/pipeline/document_source.h"

#include "mongo/db/clientcursor.h"
#include "mongo/db/instance.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/s/d_logic.h"

namespace mongo {

    DocumentSourceCursor::CursorWithContext::CursorWithContext( const string& ns )
        : _readContext( ns ) // Take a read lock.
        , _chunkMgr(shardingState.needShardChunkManager( ns )
                    ? shardingState.getShardChunkManager( ns )
                    : ShardChunkManagerPtr())
    {}

    DocumentSourceCursor::~DocumentSourceCursor() {
    }

    bool DocumentSourceCursor::eof() {
        /* if we haven't gotten the first one yet, do so now */
        if (!pCurrent.get())
            findNext();

        return (pCurrent.get() == NULL);
    }

    bool DocumentSourceCursor::advance() {
        DocumentSource::advance(); // check for interrupts

        /* if we haven't gotten the first one yet, do so now */
        if (!pCurrent.get())
            findNext();

        findNext();
        return (pCurrent.get() != NULL);
    }

    intrusive_ptr<Document> DocumentSourceCursor::getCurrent() {
        /* if we haven't gotten the first one yet, do so now */
        if (!pCurrent.get())
            findNext();

        return pCurrent;
    }

    void DocumentSourceCursor::dispose() {
        _cursorWithContext.reset();
    }

    ClientCursor::Holder& DocumentSourceCursor::cursor() {
        verify( _cursorWithContext );
        verify( _cursorWithContext->_cursor );
        return _cursorWithContext->_cursor;
    }

    bool DocumentSourceCursor::canUseCoveredIndex() {
        // We can't use a covered index when we have a chunk manager because we
        // need to examine the object to see if it belongs on this shard
        return (!chunkMgr() &&
                cursor()->ok() && cursor()->c()->keyFieldsOnly());
    }

    void DocumentSourceCursor::yieldSometimes() {
        try { // SERVER-5752 may make this try unnecessary
            // if we are index only we don't need the recored
            bool cursorOk = cursor()->yieldSometimes(canUseCoveredIndex()
                                                     ? ClientCursor::DontNeed
                                                     : ClientCursor::WillNeed);
            uassert( 16028, "collection or database disappeared when cursor yielded", cursorOk );
        }
        catch(SendStaleConfigException& e){
            // We want to ignore this because the migrated documents will be filtered out of the
            // cursor anyway and, we don't want to restart the aggregation after every migration.

            log() << "Config changed during aggregation - command will resume" << endl;
            // useful for debugging but off by default to avoid looking like a scary error.
            LOG(1) << "aggregation stale config exception: " << e.what() << endl;
        }
    }

    void DocumentSourceCursor::findNext() {

        if ( !_cursorWithContext ) {
            pCurrent.reset();
            return;
        }

        for( ; cursor()->ok(); cursor()->advance() ) {

            yieldSometimes();
            if ( !cursor()->ok() ) {
                // The cursor was exhausted during the yield.
                break;
            }

            if ( !cursor()->currentMatches() || cursor()->currentIsDup() )
                continue;

            // grab the matching document
            BSONObj documentObj;
            if (canUseCoveredIndex()) {
                // Can't have a Chunk Manager if we are here
                documentObj = cursor()->c()->keyFieldsOnly()->hydrate(cursor()->currKey());
            }
            else {
                documentObj = cursor()->current();

                // check to see if this is a new object we don't own yet
                // because of a chunk migration
                if ( chunkMgr() && ! chunkMgr()->belongsToMe(documentObj) )
                    continue;

                if (_projection) {
                    documentObj = _projection->transform(documentObj);
                }
            }

            pCurrent = Document::createFromBsonObj(&documentObj);

            cursor()->advance();
            return;
        }

        // If we got here, there aren't any more documents.
        // The CursorWithContext (and its read lock) must be released, see SERVER-6123.
        dispose();
        pCurrent.reset();
    }

    void DocumentSourceCursor::setSource(DocumentSource *pSource) {
        /* this doesn't take a source */
        verify(false);
    }

    void DocumentSourceCursor::sourceToBson(
        BSONObjBuilder *pBuilder, bool explain) const {

        /* this has no analog in the BSON world, so only allow it for explain */
        if (explain)
        {
            BSONObj bsonObj;
            
            pBuilder->append("query", *pQuery);

            if (pSort.get())
            {
                pBuilder->append("sort", *pSort);
            }

            BSONObj projectionSpec;
            if (_projection) {
                projectionSpec = _projection->getSpec();
                pBuilder->append("projection", projectionSpec);
            }

            // construct query for explain
            BSONObjBuilder queryBuilder;
            queryBuilder.append("$query", *pQuery);
            if (pSort.get())
                queryBuilder.append("$orderby", *pSort);
            queryBuilder.append("$explain", 1);
            Query query(queryBuilder.obj());

            DBDirectClient directClient;
            BSONObj explainResult(directClient.findOne(ns, query, _projection
                                                                  ? &projectionSpec
                                                                  : NULL));

            pBuilder->append("cursor", explainResult);
        }
    }

    DocumentSourceCursor::DocumentSourceCursor(
        const shared_ptr<CursorWithContext>& cursorWithContext,
        const intrusive_ptr<ExpressionContext> &pCtx):
        DocumentSource(pCtx),
        pCurrent(),
        _cursorWithContext( cursorWithContext )
    {}

    intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
        const shared_ptr<CursorWithContext>& cursorWithContext,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        verify( cursorWithContext );
        verify( cursorWithContext->_cursor );
        intrusive_ptr<DocumentSourceCursor> pSource(
            new DocumentSourceCursor( cursorWithContext, pExpCtx ) );
        return pSource;
    }

    void DocumentSourceCursor::setNamespace(const string &n) {
        ns = n;
    }

    void DocumentSourceCursor::setQuery(const shared_ptr<BSONObj> &pBsonObj) {
        pQuery = pBsonObj;
    }

    void DocumentSourceCursor::setSort(const shared_ptr<BSONObj> &pBsonObj) {
        pSort = pBsonObj;
    }

    void DocumentSourceCursor::setProjection(BSONObj projection) {
        verify(!_projection);
        _projection.reset(new Projection);
        _projection->init(projection);
        cursor()->fields = _projection;
    }
}
