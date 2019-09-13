
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

#include <memory>

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/count.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageCount {

using std::unique_ptr;
using std::vector;

const int kDocuments = 100;
const int kInterjections = kDocuments;

class CountStageTest {
public:
    CountStageTest()
        : _dbLock(&_opCtx, nsToDatabaseSubstring(ns()), MODE_X), _ctx(&_opCtx, ns()), _coll(NULL) {}

    virtual ~CountStageTest() {}

    virtual void interject(CountStage&, int) {}

    virtual void setup() {
        WriteUnitOfWork wunit(&_opCtx);

        _ctx.db()->dropCollection(&_opCtx, ns()).transitional_ignore();
        _coll = _ctx.db()->createCollection(&_opCtx, ns());

        _coll->getIndexCatalog()
            ->createIndexOnEmptyCollection(&_opCtx,
                                           BSON("key" << BSON("x" << 1) << "name"
                                                      << "x_1"
                                                      << "ns"
                                                      << ns()
                                                      << "v"
                                                      << 1))
            .status_with_transitional_ignore();

        for (int i = 0; i < kDocuments; i++) {
            insert(BSON(GENOID << "x" << i));
        }

        wunit.commit();
    }

    void getRecordIds() {
        _recordIds.clear();
        WorkingSet ws;

        CollectionScanParams params;
        params.collection = _coll;
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = false;

        unique_ptr<CollectionScan> scan(new CollectionScan(&_opCtx, params, &ws, NULL));
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                verify(member->hasRecordId());
                _recordIds.push_back(member->recordId);
            }
        }
    }

    void insert(const BSONObj& doc) {
        WriteUnitOfWork wunit(&_opCtx);
        OpDebug* const nullOpDebug = nullptr;
        _coll->insertDocument(&_opCtx, InsertStatement(doc), nullOpDebug, false)
            .transitional_ignore();
        wunit.commit();
    }

    void remove(const RecordId& recordId) {
        WriteUnitOfWork wunit(&_opCtx);
        OpDebug* const nullOpDebug = nullptr;
        _coll->deleteDocument(&_opCtx, kUninitializedStmtId, recordId, nullOpDebug);
        wunit.commit();
    }

    void update(const RecordId& oldrecordId, const BSONObj& newDoc) {
        WriteUnitOfWork wunit(&_opCtx);
        BSONObj oldDoc = _coll->getRecordStore()->dataFor(&_opCtx, oldrecordId).releaseToBson();
        OplogUpdateEntryArgs args;
        args.nss = _coll->ns();
        _coll->updateDocument(&_opCtx,
                              oldrecordId,
                              Snapshotted<BSONObj>(_opCtx.recoveryUnit()->getSnapshotId(), oldDoc),
                              newDoc,
                              false,
                              true,
                              NULL,
                              &args);
        wunit.commit();
    }

    // testcount is a wrapper around runCount that
    //  - sets up a countStage
    //  - runs it
    //  - asserts count is not trivial
    //  - asserts nCounted is equal to expected_n
    //  - asserts nSkipped is correct
    void testCount(const CountRequest& request, int expected_n = kDocuments, bool indexed = false) {
        setup();
        getRecordIds();

        unique_ptr<WorkingSet> ws(new WorkingSet);

        const CollatorInterface* collator = nullptr;
        const boost::intrusive_ptr<ExpressionContext> expCtx(
            new ExpressionContext(&_opCtx, collator));
        StatusWithMatchExpression statusWithMatcher =
            MatchExpressionParser::parse(request.getQuery(), expCtx);
        ASSERT(statusWithMatcher.isOK());
        unique_ptr<MatchExpression> expression = std::move(statusWithMatcher.getValue());

        PlanStage* scan;
        if (indexed) {
            scan = createIndexScan(expression.get(), ws.get());
        } else {
            scan = createCollScan(expression.get(), ws.get());
        }

        const bool useRecordStoreCount = false;
        CountStageParams params(request, useRecordStoreCount);
        CountStage countStage(&_opCtx, _coll, std::move(params), ws.get(), scan);

        const CountStats* stats = runCount(countStage);

        ASSERT_FALSE(stats->recordStoreCount);
        ASSERT_EQUALS(stats->nCounted, expected_n);
        ASSERT_EQUALS(stats->nSkipped, request.getSkip());
    }

    // Performs a test using a count stage whereby each unit of work is interjected
    // in some way by the invocation of interject().
    const CountStats* runCount(CountStage& count_stage) {
        int interjection = 0;
        WorkingSetID wsid;

        while (!count_stage.isEOF()) {
            // do some work -- assumes that one work unit counts a single doc
            PlanStage::StageState state = count_stage.work(&wsid);
            ASSERT_NOT_EQUALS(state, PlanStage::FAILURE);
            ASSERT_NOT_EQUALS(state, PlanStage::DEAD);

            // prepare for yield
            count_stage.saveState();

            // interject in some way kInterjection times
            if (interjection < kInterjections) {
                interject(count_stage, interjection++);
            }

            // resume from yield
            count_stage.restoreState();
        }

        return static_cast<const CountStats*>(count_stage.getSpecificStats());
    }

    IndexScan* createIndexScan(MatchExpression* expr, WorkingSet* ws) {
        IndexCatalog* catalog = _coll->getIndexCatalog();
        std::vector<IndexDescriptor*> indexes;
        catalog->findIndexesByKeyPattern(&_opCtx, BSON("x" << 1), false, &indexes);
        ASSERT_EQ(indexes.size(), 1U);
        IndexDescriptor* descriptor = indexes[0];

        // We are not testing indexing here so use maximal bounds
        IndexScanParams params;
        params.descriptor = descriptor;
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 0);
        params.bounds.endKey = BSON("" << kDocuments + 1);
        params.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
        params.direction = 1;

        // This child stage gets owned and freed by its parent CountStage
        return new IndexScan(&_opCtx, params, ws, expr);
    }

    CollectionScan* createCollScan(MatchExpression* expr, WorkingSet* ws) {
        CollectionScanParams params;
        params.collection = _coll;

        // This child stage gets owned and freed by its parent CountStage
        return new CollectionScan(&_opCtx, params, ws, expr);
    }

    static const char* ns() {
        return "unittest.QueryStageCount";
    }

protected:
    vector<RecordId> _recordIds;
    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    Lock::DBLock _dbLock;
    OldClientContext _ctx;
    Collection* _coll;
};

class QueryStageCountNoChangeDuringYield : public CountStageTest {
public:
    void run() {
        CountRequest request(NamespaceString(ns()), BSON("x" << LT << kDocuments / 2));

        testCount(request, kDocuments / 2);
        testCount(request, kDocuments / 2, true);
    }
};

class QueryStageCountYieldWithSkip : public CountStageTest {
public:
    void run() {
        CountRequest request(NamespaceString(ns()), BSON("x" << GTE << 0));
        request.setSkip(2);

        testCount(request, kDocuments - 2);
        testCount(request, kDocuments - 2, true);
    }
};

class QueryStageCountYieldWithLimit : public CountStageTest {
public:
    void run() {
        CountRequest request(NamespaceString(ns()), BSON("x" << GTE << 0));
        request.setSkip(0);
        request.setLimit(2);

        testCount(request, 2);
        testCount(request, 2, true);
    }
};


class QueryStageCountInsertDuringYield : public CountStageTest {
public:
    void run() {
        CountRequest request(NamespaceString(ns()), BSON("x" << 1));

        testCount(request, kInterjections + 1);
        testCount(request, kInterjections + 1, true);
    }

    // This is called 100 times as we scan the collection
    void interject(CountStage&, int) {
        insert(BSON(GENOID << "x" << 1));
    }
};

class QueryStageCountDeleteDuringYield : public CountStageTest {
public:
    void run() {
        // expected count would be 99 but we delete the second record
        // after doing the first unit of work
        CountRequest request(NamespaceString(ns()), BSON("x" << GTE << 1));

        testCount(request, kDocuments - 2);
        testCount(request, kDocuments - 2, true);
    }

    // At the point which this is called we are in between counting the first + second record
    void interject(CountStage& count_stage, int interjection) {
        if (interjection == 0) {
            // At this point, our first interjection, we've counted _recordIds[0]
            // and are about to count _recordIds[1]
            WriteUnitOfWork wunit(&_opCtx);
            count_stage.invalidate(&_opCtx, _recordIds[interjection], INVALIDATION_DELETION);
            remove(_recordIds[interjection]);

            count_stage.invalidate(&_opCtx, _recordIds[interjection + 1], INVALIDATION_DELETION);
            remove(_recordIds[interjection + 1]);
            wunit.commit();
        }
    }
};

class QueryStageCountUpdateDuringYield : public CountStageTest {
public:
    void run() {
        // expected count would be kDocuments-2 but we update the first and second records
        // after doing the first unit of work so they wind up getting counted later on
        CountRequest request(NamespaceString(ns()), BSON("x" << GTE << 2));

        testCount(request, kDocuments);
        testCount(request, kDocuments, true);
    }

    // At the point which this is called we are in between the first and second record
    void interject(CountStage& count_stage, int interjection) {
        if (interjection == 0) {
            count_stage.invalidate(&_opCtx, _recordIds[0], INVALIDATION_MUTATION);
            OID id1 = _coll->docFor(&_opCtx, _recordIds[0]).value().getField("_id").OID();
            update(_recordIds[0], BSON("_id" << id1 << "x" << 100));

            count_stage.invalidate(&_opCtx, _recordIds[1], INVALIDATION_MUTATION);
            OID id2 = _coll->docFor(&_opCtx, _recordIds[1]).value().getField("_id").OID();
            update(_recordIds[1], BSON("_id" << id2 << "x" << 100));
        }
    }
};

class QueryStageCountMultiKeyDuringYield : public CountStageTest {
public:
    void run() {
        CountRequest request(NamespaceString(ns()), BSON("x" << 1));
        testCount(request, kDocuments + 1, true);  // only applies to indexed case
    }

    void interject(CountStage&, int) {
        // Should cause index to be converted to multikey
        insert(BSON(GENOID << "x" << BSON_ARRAY(1 << 2)));
    }
};

class All : public Suite {
public:
    All() : Suite("query_stage_count") {}

    void setupTests() {
        add<QueryStageCountNoChangeDuringYield>();
        add<QueryStageCountYieldWithSkip>();
        add<QueryStageCountYieldWithLimit>();
        add<QueryStageCountInsertDuringYield>();
        add<QueryStageCountDeleteDuringYield>();
        add<QueryStageCountUpdateDuringYield>();
        add<QueryStageCountMultiKeyDuringYield>();
    }
} QueryStageCountAll;

}  // namespace QueryStageCount
