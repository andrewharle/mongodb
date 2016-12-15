/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"

/**
 * This file tests db/exec/sort.cpp
 */

namespace QueryStageSortTests {

using std::set;
using std::unique_ptr;
using stdx::make_unique;

class QueryStageSortTestBase {
public:
    QueryStageSortTestBase() : _client(&_txn) {}

    void fillData() {
        for (int i = 0; i < numObj(); ++i) {
            insert(BSON("foo" << i));
        }
    }

    virtual ~QueryStageSortTestBase() {
        _client.dropCollection(ns());
    }

    void insert(const BSONObj& obj) {
        _client.insert(ns(), obj);
    }

    void getLocs(set<RecordId>* out, Collection* coll) {
        auto cursor = coll->getCursor(&_txn);
        while (auto record = cursor->next()) {
            out->insert(record->id);
        }
    }

    /**
     * We feed a mix of (key, unowned, owned) data to the sort stage.
     */
    void insertVarietyOfObjects(WorkingSet* ws, QueuedDataStage* ms, Collection* coll) {
        set<RecordId> locs;
        getLocs(&locs, coll);

        set<RecordId>::iterator it = locs.begin();

        for (int i = 0; i < numObj(); ++i, ++it) {
            ASSERT_FALSE(it == locs.end());

            // Insert some owned obj data.
            WorkingSetID id = ws->allocate();
            WorkingSetMember* member = ws->get(id);
            member->loc = *it;
            member->obj = coll->docFor(&_txn, *it);
            ws->transitionToLocAndObj(id);
            ms->pushBack(id);
        }
    }

    /*
     * Wraps a sort stage with a QueuedDataStage in a plan executor. Returns the plan executor,
     * which is owned by the caller.
     */
    PlanExecutor* makePlanExecutorWithSortStage(Collection* coll) {
        // Build the mock scan stage which feeds the data.
        auto ws = make_unique<WorkingSet>();
        auto queuedDataStage = make_unique<QueuedDataStage>(&_txn, ws.get());
        insertVarietyOfObjects(ws.get(), queuedDataStage.get(), coll);

        SortStageParams params;
        params.collection = coll;
        params.pattern = BSON("foo" << 1);
        params.limit = limit();

        auto keyGenStage = make_unique<SortKeyGeneratorStage>(
            &_txn, queuedDataStage.release(), ws.get(), params.pattern, BSONObj());

        auto ss = make_unique<SortStage>(&_txn, params, ws.get(), keyGenStage.release());

        // The PlanExecutor will be automatically registered on construction due to the auto
        // yield policy, so it can receive invalidations when we remove documents later.
        auto statusWithPlanExecutor =
            PlanExecutor::make(&_txn, std::move(ws), std::move(ss), coll, PlanExecutor::YIELD_AUTO);
        invariant(statusWithPlanExecutor.isOK());
        return statusWithPlanExecutor.getValue().release();
    }

    // Return a value in the set {-1, 0, 1} to represent the sign of parameter i.  Used to
    // normalize woCompare calls.
    int sgn(int i) {
        if (i == 0)
            return 0;
        return i > 0 ? 1 : -1;
    }

    /**
     * A template used by many tests below.
     * Fill out numObj objects, sort them in the order provided by 'direction'.
     * If extAllowed is true, sorting will use use external sorting if available.
     * If limit is not zero, we limit the output of the sort stage to 'limit' results.
     */
    void sortAndCheck(int direction, Collection* coll) {
        auto ws = make_unique<WorkingSet>();
        auto queuedDataStage = make_unique<QueuedDataStage>(&_txn, ws.get());

        // Insert a mix of the various types of data.
        insertVarietyOfObjects(ws.get(), queuedDataStage.get(), coll);

        SortStageParams params;
        params.collection = coll;
        params.pattern = BSON("foo" << direction);
        params.limit = limit();

        auto keyGenStage = make_unique<SortKeyGeneratorStage>(
            &_txn, queuedDataStage.release(), ws.get(), params.pattern, BSONObj());

        auto sortStage = make_unique<SortStage>(&_txn, params, ws.get(), keyGenStage.release());

        auto fetchStage =
            make_unique<FetchStage>(&_txn, ws.get(), sortStage.release(), nullptr, coll);

        // Must fetch so we can look at the doc as a BSONObj.
        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(fetchStage), coll, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        // Look at pairs of objects to make sure that the sort order is pairwise (and therefore
        // totally) correct.
        BSONObj last;
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&last, NULL));
        last = last.getOwned();

        // Count 'last'.
        int count = 1;

        BSONObj current;
        while (PlanExecutor::ADVANCED == exec->getNext(&current, NULL)) {
            int cmp = sgn(current.woSortOrder(last, params.pattern));
            // The next object should be equal to the previous or oriented according to the sort
            // pattern.
            ASSERT(cmp == 0 || cmp == 1);
            ++count;
            last = current.getOwned();
        }

        checkCount(count);
    }

    /**
     * Check number of results returned from sort.
     */
    void checkCount(int count) {
        // No limit, should get all objects back.
        // Otherwise, result set should be smaller of limit and input data size.
        if (limit() > 0 && limit() < numObj()) {
            ASSERT_EQUALS(limit(), count);
        } else {
            ASSERT_EQUALS(numObj(), count);
        }
    }

    virtual int numObj() = 0;

    // Returns sort limit
    // Leave as 0 to disable limit.
    virtual int limit() const {
        return 0;
    };


    static const char* ns() {
        return "unittests.QueryStageSort";
    }

protected:
    OperationContextImpl _txn;
    DBDirectClient _client;
};


// Sort some small # of results in increasing order.
class QueryStageSortInc : public QueryStageSortTestBase {
public:
    virtual int numObj() {
        return 100;
    }

    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        fillData();
        sortAndCheck(1, coll);
    }
};

// Sort some small # of results in decreasing order.
class QueryStageSortDec : public QueryStageSortTestBase {
public:
    virtual int numObj() {
        return 100;
    }

    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        fillData();
        sortAndCheck(-1, coll);
    }
};

// Sort in descreasing order with limit applied
template <int LIMIT>
class QueryStageSortDecWithLimit : public QueryStageSortDec {
public:
    virtual int limit() const {
        return LIMIT;
    }
};

// Sort a big bunch of objects.
class QueryStageSortExt : public QueryStageSortTestBase {
public:
    virtual int numObj() {
        return 10000;
    }

    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        fillData();
        sortAndCheck(-1, coll);
    }
};

// Mutation invalidation of docs fed to sort.
class QueryStageSortMutationInvalidation : public QueryStageSortTestBase {
public:
    virtual int numObj() {
        return 2000;
    }
    virtual int limit() const {
        return 10;
    }

    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        {
            WriteUnitOfWork wuow(&_txn);
            fillData();
            wuow.commit();
        }

        // The data we're going to later invalidate.
        set<RecordId> locs;
        getLocs(&locs, coll);

        unique_ptr<PlanExecutor> exec(makePlanExecutorWithSortStage(coll));
        SortStage* ss = static_cast<SortStage*>(exec->getRootStage());
        SortKeyGeneratorStage* keyGenStage =
            static_cast<SortKeyGeneratorStage*>(ss->getChildren()[0].get());
        QueuedDataStage* queuedDataStage =
            static_cast<QueuedDataStage*>(keyGenStage->getChildren()[0].get());

        // Have sort read in data from the queued data stage.
        const int firstRead = 5;
        for (int i = 0; i < firstRead; ++i) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ss->work(&id);
            ASSERT_NOT_EQUALS(PlanStage::ADVANCED, status);
        }

        // We should have read in the first 'firstRead' locs.  Invalidate the first one.
        // Since it's in the WorkingSet, the updates should not be reflected in the output.
        exec->saveState();
        set<RecordId>::iterator it = locs.begin();
        Snapshotted<BSONObj> oldDoc = coll->docFor(&_txn, *it);

        OID updatedId = oldDoc.value().getField("_id").OID();
        SnapshotId idBeforeUpdate = oldDoc.snapshotId();
        // We purposefully update the document to have a 'foo' value greater than limit().
        // This allows us to check that we don't return the new copy of a doc by asserting
        // foo < limit().
        BSONObj newDoc = BSON("_id" << updatedId << "foo" << limit() + 10);
        oplogUpdateEntryArgs args;
        {
            WriteUnitOfWork wuow(&_txn);
            coll->updateDocument(&_txn, *it, oldDoc, newDoc, false, false, NULL, args);
            wuow.commit();
        }
        exec->restoreState();

        // Read the rest of the data from the queued data stage.
        while (!queuedDataStage->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            ss->work(&id);
        }

        // Let's just invalidate everything now. Already read into ss, so original values
        // should be fetched.
        exec->saveState();
        while (it != locs.end()) {
            oldDoc = coll->docFor(&_txn, *it);
            {
                WriteUnitOfWork wuow(&_txn);
                coll->updateDocument(&_txn, *it++, oldDoc, newDoc, false, false, NULL, args);
                wuow.commit();
            }
        }
        exec->restoreState();

        // Verify that it's sorted, the right number of documents are returned, and they're all
        // in the expected range.
        int count = 0;
        int lastVal = 0;
        int thisVal;
        while (!ss->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ss->work(&id);
            if (PlanStage::ADVANCED != status) {
                ASSERT_NE(status, PlanStage::FAILURE);
                ASSERT_NE(status, PlanStage::DEAD);
                continue;
            }
            WorkingSetMember* member = exec->getWorkingSet()->get(id);
            ASSERT(member->hasObj());
            if (member->obj.value().getField("_id").OID() == updatedId) {
                ASSERT(idBeforeUpdate == member->obj.snapshotId());
            }
            thisVal = member->obj.value().getField("foo").Int();
            ASSERT_LTE(lastVal, thisVal);
            // Expect docs in range [0, limit)
            ASSERT_LTE(0, thisVal);
            ASSERT_LT(thisVal, limit());
            lastVal = thisVal;
            ++count;
        }
        // Returns all docs.
        ASSERT_EQUALS(limit(), count);
    }
};

// Deletion invalidation of everything fed to sort.
class QueryStageSortDeletionInvalidation : public QueryStageSortTestBase {
public:
    virtual int numObj() {
        return 2000;
    }

    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        {
            WriteUnitOfWork wuow(&_txn);
            fillData();
            wuow.commit();
        }

        // The data we're going to later invalidate.
        set<RecordId> locs;
        getLocs(&locs, coll);

        unique_ptr<PlanExecutor> exec(makePlanExecutorWithSortStage(coll));
        SortStage* ss = static_cast<SortStage*>(exec->getRootStage());
        SortKeyGeneratorStage* keyGenStage =
            static_cast<SortKeyGeneratorStage*>(ss->getChildren()[0].get());
        QueuedDataStage* queuedDataStage =
            static_cast<QueuedDataStage*>(keyGenStage->getChildren()[0].get());

        const int firstRead = 10;
        // Have sort read in data from the queued data stage.
        for (int i = 0; i < firstRead; ++i) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ss->work(&id);
            ASSERT_NOT_EQUALS(PlanStage::ADVANCED, status);
        }

        // We should have read in the first 'firstRead' locs.  Invalidate the first.
        exec->saveState();
        set<RecordId>::iterator it = locs.begin();
        {
            WriteUnitOfWork wuow(&_txn);
            coll->deleteDocument(&_txn, *it++);
            wuow.commit();
        }
        exec->restoreState();

        // Read the rest of the data from the queued data stage.
        while (!queuedDataStage->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            ss->work(&id);
        }

        // Let's just invalidate everything now.
        exec->saveState();
        while (it != locs.end()) {
            {
                WriteUnitOfWork wuow(&_txn);
                coll->deleteDocument(&_txn, *it++);
                wuow.commit();
            }
        }
        exec->restoreState();

        // Regardless of storage engine, all the documents should come back with their objects
        int count = 0;
        while (!ss->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ss->work(&id);
            if (PlanStage::ADVANCED != status) {
                ASSERT_NE(status, PlanStage::FAILURE);
                ASSERT_NE(status, PlanStage::DEAD);
                continue;
            }
            WorkingSetMember* member = exec->getWorkingSet()->get(id);
            ASSERT(member->hasObj());
            ++count;
        }

        // Returns all docs.
        ASSERT_EQUALS(limit() ? limit() : numObj(), count);
    }
};

// Deletion invalidation of everything fed to sort with limit enabled.
// Limit size of working set within sort stage to a small number
// Sort stage implementation should not try to invalidate DiskLocc that
// are no longer in the working set.

template <int LIMIT>
class QueryStageSortDeletionInvalidationWithLimit : public QueryStageSortDeletionInvalidation {
public:
    virtual int limit() const {
        return LIMIT;
    }
};

// Should error out if we sort with parallel arrays.
class QueryStageSortParallelArrays : public QueryStageSortTestBase {
public:
    virtual int numObj() {
        return 100;
    }

    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        auto ws = make_unique<WorkingSet>();
        auto queuedDataStage = make_unique<QueuedDataStage>(&_txn, ws.get());

        for (int i = 0; i < numObj(); ++i) {
            {
                WorkingSetID id = ws->allocate();
                WorkingSetMember* member = ws->get(id);
                member->obj = Snapshotted<BSONObj>(
                    SnapshotId(), fromjson("{a: [1,2,3], b:[1,2,3], c:[1,2,3], d:[1,2,3,4]}"));
                member->transitionToOwnedObj();
                queuedDataStage->pushBack(id);
            }
            {
                WorkingSetID id = ws->allocate();
                WorkingSetMember* member = ws->get(id);
                member->obj = Snapshotted<BSONObj>(SnapshotId(), fromjson("{a:1, b:1, c:1}"));
                member->transitionToOwnedObj();
                queuedDataStage->pushBack(id);
            }
        }

        SortStageParams params;
        params.collection = coll;
        params.pattern = BSON("b" << -1 << "c" << 1 << "a" << 1);
        params.limit = 0;

        auto keyGenStage = make_unique<SortKeyGeneratorStage>(
            &_txn, queuedDataStage.release(), ws.get(), params.pattern, BSONObj());

        auto sortStage = make_unique<SortStage>(&_txn, params, ws.get(), keyGenStage.release());

        auto fetchStage =
            make_unique<FetchStage>(&_txn, ws.get(), sortStage.release(), nullptr, coll);

        // We don't get results back since we're sorting some parallel arrays.
        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(fetchStage), coll, PlanExecutor::YIELD_MANUAL);
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        PlanExecutor::ExecState runnerState = exec->getNext(NULL, NULL);
        ASSERT_EQUALS(PlanExecutor::FAILURE, runnerState);
    }
};

class All : public Suite {
public:
    All() : Suite("query_stage_sort") {}

    void setupTests() {
        add<QueryStageSortInc>();
        add<QueryStageSortDec>();
        // Sort with limit has a general limiting strategy for limit > 1
        add<QueryStageSortDecWithLimit<10>>();
        // and a special case for limit == 1
        add<QueryStageSortDecWithLimit<1>>();
        add<QueryStageSortExt>();
        add<QueryStageSortMutationInvalidation>();
        add<QueryStageSortDeletionInvalidation>();
        add<QueryStageSortDeletionInvalidationWithLimit<10>>();
        add<QueryStageSortDeletionInvalidationWithLimit<1>>();
        add<QueryStageSortParallelArrays>();
    }
};

SuiteInstance<All> queryStageSortTest;

}  // namespace
