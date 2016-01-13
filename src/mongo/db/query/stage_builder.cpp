/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/stage_builder.h"

#include "mongo/db/exec/2d.h"
#include "mongo/db/exec/2dnear.h"
#include "mongo/db/exec/and_hash.h"
#include "mongo/db/exec/and_sorted.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/count.h"
#include "mongo/db/exec/distinct_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/keep_mutations.h"
#include "mongo/db/exec/limit.h"
#include "mongo/db/exec/merge_sort.h"
#include "mongo/db/exec/or.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/s2near.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/skip.h"
#include "mongo/db/exec/text.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/catalog/collection.h"

namespace mongo {

    PlanStage* buildStages(const QuerySolution& qsol, const QuerySolutionNode* root, WorkingSet* ws) {
        if (STAGE_COLLSCAN == root->getType()) {
            const CollectionScanNode* csn = static_cast<const CollectionScanNode*>(root);
            CollectionScanParams params;
            params.ns = csn->name;
            params.tailable = csn->tailable;
            params.direction = (csn->direction == 1) ? CollectionScanParams::FORWARD
                                                     : CollectionScanParams::BACKWARD;
            params.maxScan = csn->maxScan;
            return new CollectionScan(params, ws, csn->filter.get());
        }
        else if (STAGE_IXSCAN == root->getType()) {
            const IndexScanNode* ixn = static_cast<const IndexScanNode*>(root);

            Database* db = cc().database();
            Collection* collection = db ? db->getCollection(qsol.ns) : NULL;
            if (NULL == collection) {
                warning() << "Can't ixscan null namespace " << qsol.ns << endl;
                return NULL;
            }

            IndexScanParams params;

            params.descriptor = collection->getIndexCatalog()->findIndexByKeyPattern( ixn->indexKeyPattern );
            if ( params.descriptor == NULL ) {
                warning() << "Can't find index " << ixn->indexKeyPattern.toString()
                          << "in namespace " << qsol.ns << endl;
                return NULL;
            }

            params.bounds = ixn->bounds;
            params.direction = ixn->direction;
            params.maxScan = ixn->maxScan;
            params.addKeyMetadata = ixn->addKeyMetadata;
            return new IndexScan(params, ws, ixn->filter.get());
        }
        else if (STAGE_FETCH == root->getType()) {
            const FetchNode* fn = static_cast<const FetchNode*>(root);
            PlanStage* childStage = buildStages(qsol, fn->children[0], ws);
            if (NULL == childStage) { return NULL; }
            return new FetchStage(ws, childStage, fn->filter.get());
        }
        else if (STAGE_SORT == root->getType()) {
            const SortNode* sn = static_cast<const SortNode*>(root);
            PlanStage* childStage = buildStages(qsol, sn->children[0], ws);
            if (NULL == childStage) { return NULL; }
            SortStageParams params;
            params.pattern = sn->pattern;
            params.query = sn->query;
            params.limit = sn->limit;
            return new SortStage(params, ws, childStage);
        }
        else if (STAGE_PROJECTION == root->getType()) {
            const ProjectionNode* pn = static_cast<const ProjectionNode*>(root);
            PlanStage* childStage = buildStages(qsol, pn->children[0], ws);
            if (NULL == childStage) { return NULL; }
            ProjectionStageParams params;
            params.projObj = pn->projection;

            // Stuff the right data into the params depending on what proj impl we use.
            if (ProjectionNode::DEFAULT == pn->projType) {
                params.fullExpression = pn->fullExpression;
                params.projImpl = ProjectionStageParams::NO_FAST_PATH;
            }
            else if (ProjectionNode::COVERED_ONE_INDEX == pn->projType) {
                params.projImpl = ProjectionStageParams::COVERED_ONE_INDEX;
                params.coveredKeyObj = pn->coveredKeyObj;
                invariant(!pn->coveredKeyObj.isEmpty());
            }
            else {
                invariant(ProjectionNode::SIMPLE_DOC == pn->projType);
                params.projImpl = ProjectionStageParams::SIMPLE_DOC;
            }

            return new ProjectionStage(params, ws, childStage);
        }
        else if (STAGE_LIMIT == root->getType()) {
            const LimitNode* ln = static_cast<const LimitNode*>(root);
            PlanStage* childStage = buildStages(qsol, ln->children[0], ws);
            if (NULL == childStage) { return NULL; }
            return new LimitStage(ln->limit, ws, childStage);
        }
        else if (STAGE_SKIP == root->getType()) {
            const SkipNode* sn = static_cast<const SkipNode*>(root);
            PlanStage* childStage = buildStages(qsol, sn->children[0], ws);
            if (NULL == childStage) { return NULL; }
            return new SkipStage(sn->skip, ws, childStage);
        }
        else if (STAGE_AND_HASH == root->getType()) {
            const AndHashNode* ahn = static_cast<const AndHashNode*>(root);
            auto_ptr<AndHashStage> ret(new AndHashStage(ws, ahn->filter.get()));
            for (size_t i = 0; i < ahn->children.size(); ++i) {
                PlanStage* childStage = buildStages(qsol, ahn->children[i], ws);
                if (NULL == childStage) { return NULL; }
                ret->addChild(childStage);
            }
            return ret.release();
        }
        else if (STAGE_OR == root->getType()) {
            const OrNode * orn = static_cast<const OrNode*>(root);
            auto_ptr<OrStage> ret(new OrStage(ws, orn->dedup, orn->filter.get()));
            for (size_t i = 0; i < orn->children.size(); ++i) {
                PlanStage* childStage = buildStages(qsol, orn->children[i], ws);
                if (NULL == childStage) { return NULL; }
                ret->addChild(childStage);
            }
            return ret.release();
        }
        else if (STAGE_AND_SORTED == root->getType()) {
            const AndSortedNode* asn = static_cast<const AndSortedNode*>(root);
            auto_ptr<AndSortedStage> ret(new AndSortedStage(ws, asn->filter.get()));
            for (size_t i = 0; i < asn->children.size(); ++i) {
                PlanStage* childStage = buildStages(qsol, asn->children[i], ws);
                if (NULL == childStage) { return NULL; }
                ret->addChild(childStage);
            }
            return ret.release();
        }
        else if (STAGE_SORT_MERGE == root->getType()) {
            const MergeSortNode* msn = static_cast<const MergeSortNode*>(root);
            MergeSortStageParams params;
            params.dedup = msn->dedup;
            params.pattern = msn->sort;
            auto_ptr<MergeSortStage> ret(new MergeSortStage(params, ws));
            for (size_t i = 0; i < msn->children.size(); ++i) {
                PlanStage* childStage = buildStages(qsol, msn->children[i], ws);
                if (NULL == childStage) { return NULL; }
                ret->addChild(childStage);
            }
            return ret.release();
        }
        else if (STAGE_GEO_2D == root->getType()) {
            const Geo2DNode* node = static_cast<const Geo2DNode*>(root);
            TwoDParams params;
            params.gq = node->gq;
            params.filter = node->filter.get();
            params.indexKeyPattern = node->indexKeyPattern;
            params.ns = qsol.ns;
            return new TwoD(params, ws);
        }
        else if (STAGE_GEO_NEAR_2D == root->getType()) {
            const GeoNear2DNode* node = static_cast<const GeoNear2DNode*>(root);
            TwoDNearParams params;
            params.nearQuery = node->nq;
            params.ns = qsol.ns;
            params.indexKeyPattern = node->indexKeyPattern;
            params.filter = node->filter.get();
            params.numWanted = node->numWanted;
            params.addPointMeta = node->addPointMeta;
            params.addDistMeta = node->addDistMeta;
            return new TwoDNear(params, ws);
        }
        else if (STAGE_GEO_NEAR_2DSPHERE == root->getType()) {
            const GeoNear2DSphereNode* node = static_cast<const GeoNear2DSphereNode*>(root);
            S2NearParams params;
            params.ns = qsol.ns;
            params.indexKeyPattern = node->indexKeyPattern;
            params.nearQuery = node->nq;
            params.baseBounds = node->baseBounds;
            params.filter = node->filter.get();
            params.addPointMeta = node->addPointMeta;
            params.addDistMeta = node->addDistMeta;
            return new S2NearStage(params, ws);
        }
        else if (STAGE_TEXT == root->getType()) {
            const TextNode* node = static_cast<const TextNode*>(root);

            Database* db = cc().database();
            Collection* collection = db ? db->getCollection(qsol.ns) : NULL;
            if (NULL == collection) {
                warning() << "Null collection for text";
                return NULL;
            }
            vector<IndexDescriptor*> idxMatches;
            collection->getIndexCatalog()->findIndexByType("text", idxMatches);
            if (1 != idxMatches.size()) {
                warning() << "No text index, or more than one text index";
                return NULL;
            }
            IndexDescriptor* index = idxMatches[0];
            const FTSAccessMethod* fam =
                static_cast<FTSAccessMethod*>( collection->getIndexCatalog()->getIndex( index ) );
            TextStageParams params(fam->getSpec());

            params.ns = qsol.ns;
            params.index = index;
            params.spec = fam->getSpec();
            params.indexPrefix = node->indexPrefix;

            const std::string& language = ("" == node->language
                                           ? fam->getSpec().defaultLanguage().str()
                                           : node->language);

            Status parseStatus = params.query.parse(node->query, language,
                                                    fam->getSpec().getTextIndexVersion());
            if (!parseStatus.isOK()) {
                warning() << "Can't parse text search query";
                return NULL;
            }

            return new TextStage(params, ws, node->filter.get());
        }
        else if (STAGE_SHARDING_FILTER == root->getType()) {
            const ShardingFilterNode* fn = static_cast<const ShardingFilterNode*>(root);
            PlanStage* childStage = buildStages(qsol, fn->children[0], ws);
            if (NULL == childStage) { return NULL; }
            return new ShardFilterStage(shardingState.getCollectionMetadata(qsol.ns), ws, childStage);
        }
        else if (STAGE_KEEP_MUTATIONS == root->getType()) {
            const KeepMutationsNode* km = static_cast<const KeepMutationsNode*>(root);
            PlanStage* childStage = buildStages(qsol, km->children[0], ws);
            if (NULL == childStage) { return NULL; }
            return new KeepMutationsStage(km->filter.get(), ws, childStage);
        }
        else if (STAGE_DISTINCT == root->getType()) {
            const DistinctNode* dn = static_cast<const DistinctNode*>(root);

            Database* db = cc().database();
            Collection* collection = db ? db->getCollection(qsol.ns) : NULL;
            if (NULL == collection) {
                warning() << "Can't distinct-scan null namespace " << qsol.ns << endl;
                return NULL;
            }

            DistinctParams params;

            params.descriptor =
                collection->getIndexCatalog()->findIndexByKeyPattern(dn->indexKeyPattern);
            params.direction = dn->direction;
            params.bounds = dn->bounds;
            params.fieldNo = dn->fieldNo;
            return new DistinctScan(params, ws);
        }
        else if (STAGE_COUNT == root->getType()) {
            const CountNode* cn = static_cast<const CountNode*>(root);

            Database* db = cc().database();
            if (NULL == db) {
                warning() << "Can't fast-count null namespace (database null)" << qsol.ns << endl;
                return NULL;
            }

            Collection* collection = db ? db->getCollection(qsol.ns) : NULL;
            if (NULL == collection) {
                warning() << "Can't fast-count null namespace (collection null)" << qsol.ns << endl;
                return NULL;
            }

            CountParams params;

            params.descriptor =
                collection->getIndexCatalog()->findIndexByKeyPattern(cn->indexKeyPattern);
            params.startKey = cn->startKey;
            params.startKeyInclusive = cn->startKeyInclusive;
            params.endKey = cn->endKey;
            params.endKeyInclusive = cn->endKeyInclusive;

            return new Count(params, ws);
        }
        else {
            mongoutils::str::stream ss;
            root->appendToString(&ss, 0);
            string nodeStr(ss);
            warning() << "Can't build exec tree for node " << nodeStr << endl;
            return NULL;
        }
    }

    // static
    bool StageBuilder::build(const QuerySolution& solution, PlanStage** rootOut,
                             WorkingSet** wsOut) {
        QuerySolutionNode* root = solution.root.get();
        if (NULL == root) { return false; }

        auto_ptr<WorkingSet> ws(new WorkingSet());
        PlanStage* stageRoot = buildStages(solution, root, ws.get());

        if (NULL != stageRoot) {
            *rootOut = stageRoot;
            *wsOut = ws.release();
            return true;
        }
        else {
            return false;
        }
    }

}  // namespace mongo
