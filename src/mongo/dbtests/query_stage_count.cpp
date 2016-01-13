/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <boost/shared_ptr.hpp>

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/keep_mutations.h"
#include "mongo/db/exec/count.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/structure/collection_iterator.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_registry.h"
#include "mongo/util/fail_point_service.h"

namespace QueryStageCount {

    class CountBase {
    public:
        CountBase() { }

        virtual ~CountBase() {
            Client::WriteContext ctx(ns());
            _client.dropCollection(ns());
        }

        void addIndex(const BSONObj& obj) {
            _client.ensureIndex(ns(), obj);
        }

        void insert(const BSONObj& obj) {
            _client.insert(ns(), obj);
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
        }

        /* 
         * Given a Count PlanStage object count, call work() on 
         * count until we reach IS_EOF.  Tally up how many objects
         * we've counted and return the count.
         */
        int runCount(Count* count) {
            int countWorks = 0;
            WorkingSetID wsid;

            PlanStage::StageState countState = count->work(&wsid);

            while (PlanStage::IS_EOF != countState) {
                if (PlanStage::ADVANCED == countState) countWorks++;
                countState = count->work(&wsid);
            }
            return countWorks;
        }

        IndexDescriptor* getIndex(Database* db, const BSONObj& obj) {
            Collection* collection = db->getCollection(ns());
            return collection->getIndexCatalog()->findIndexByKeyPattern(obj);
        }

        static const char* ns() { return "unittests.QueryStageCount"; }

    private:
        static DBDirectClient _client;
    };
    
    DBDirectClient CountBase::_client;

    //
    // Check that dups are properly identified
    //
    class QueryStageCountDups : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert some docs
            insert(BSON("a" << BSON_ARRAY(5 << 7)));
            insert(BSON("a" << BSON_ARRAY(6 << 8)));

            // Add an index on a:1
            addIndex(BSON("a" << 1));

            // Set up the count stage
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            verify(params.descriptor);
            params.startKey = BSON("a" << 1);
            params.startKeyInclusive = true;
            params.endKey = BSON("a" << 10);
            params.endKeyInclusive = true;

            WorkingSet ws;
            Count count(params, &ws);

            int numCounted = runCount(&count);
            ASSERT_EQUALS(2, numCounted);
        }
    };

    //
    // Check that expected results are returned with inclusive bounds
    //
    class QueryStageCountInclusiveBounds : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert some docs
            for (int i = 0; i < 10; ++i) {
                insert(BSON("a" << i));
            }

            // Add an index
            addIndex(BSON("a" << 1));
 
            // Set up the count stage
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 3);
            params.startKeyInclusive = true;
            params.endKey = BSON("" << 7);
            params.endKeyInclusive = true;

            WorkingSet ws;
            Count count(params, &ws);

            int numCounted = runCount(&count);
            ASSERT_EQUALS(5, numCounted);
        }
    };

    //
    // Check that expected results are returned with exclusive bounds
    //
    class QueryStageCountExclusiveBounds : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert some docs
            for (int i = 0; i < 10; ++i) {
                insert(BSON("a" << i));
            }

            // Add an index
            addIndex(BSON("a" << 1));
 
            // Set up the count stage
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 3);
            params.startKeyInclusive = false;
            params.endKey = BSON("" << 7);
            params.endKeyInclusive = false;

            WorkingSet ws;
            Count count(params, &ws);

            int numCounted = runCount(&count);
            ASSERT_EQUALS(3, numCounted);
        }
    };

    //
    // Check that cursor returns no results if all docs are below lower bound
    //
    class QueryStageCountLowerBound : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert doc, add index
            insert(BSON("a" << 2));
            addIndex(BSON("a" << 1));

            // Set up count, and run
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 2);
            params.startKeyInclusive = false;
            params.endKey = BSON("" << 3);
            params.endKeyInclusive = false;

            WorkingSet ws;
            Count count(params, &ws);
            
            int numCounted = runCount(&count);
            ASSERT_EQUALS(0, numCounted);
        }
    };

    //
    // Check that cursor returns no results if there are no docs within interval
    //
    class QueryStageCountNothingInInterval : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert documents, add index
            insert(BSON("a" << 2));
            insert(BSON("a" << 3));
            addIndex(BSON("a" << 1));

            // Set up count, and run
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 2);
            params.startKeyInclusive = false;
            params.endKey = BSON("" << 3);
            params.endKeyInclusive = false;

            WorkingSet ws;
            Count count(params, &ws);
            
            int numCounted = runCount(&count);
            ASSERT_EQUALS(0, numCounted);
        }
    };

    //
    // Check that cursor returns no results if there are no docs within interval
    // and the first key located during initialization is above upper bound
    //
    class QueryStageCountNothingInIntervalFirstMatchTooHigh : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert some documents, add index
            insert(BSON("a" << 2));
            insert(BSON("a" << 4));
            addIndex(BSON("a" << 1));

            // Set up count, and run
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 2);
            params.startKeyInclusive = false;
            params.endKey = BSON("" << 3);
            params.endKeyInclusive = true;

            WorkingSet ws;
            Count count(params, &ws);
            
            int numCounted = runCount(&count);
            ASSERT_EQUALS(0, numCounted);
        }
    };

    //
    // Check that cursor recovers its position properly if there is no change
    // during a yield
    //
    class QueryStageCountNoChangeDuringYield : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert documents, add index
            for (int i = 0; i < 10; ++i) {
                insert(BSON("a" << i));
            }
            addIndex(BSON("a" << 1));

            // Set up count stage
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 2);
            params.startKeyInclusive = false;
            params.endKey = BSON("" << 6);
            params.endKeyInclusive = true;

            WorkingSet ws;
            Count count(params, &ws);
            WorkingSetID wsid;

            int numCounted = 0;
            PlanStage::StageState countState;

            // Begin running the count
            while (numCounted < 2) {
                countState = count.work(&wsid);
                if (PlanStage::ADVANCED == countState) numCounted++;
            }

            // Prepare the cursor to yield
            count.prepareToYield();

            // Recover from yield
            count.recoverFromYield();

            // finish counting
            while (PlanStage::IS_EOF != countState) {
                countState = count.work(&wsid);
                if (PlanStage::ADVANCED == countState) numCounted++;
            }
            ASSERT_EQUALS(4, numCounted);
        }
    };

    //
    // Check that cursor recovers its position properly if its current location
    // is deleted during a yield
    //
    class QueryStageCountDeleteDuringYield : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert documents, add index
            for (int i = 0; i < 10; ++i) {
                insert(BSON("a" << i));
            }
            addIndex(BSON("a" << 1));

            // Set up count stage
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 2);
            params.startKeyInclusive = false;
            params.endKey = BSON("" << 6);
            params.endKeyInclusive = true;

            WorkingSet ws;
            Count count(params, &ws);
            WorkingSetID wsid;

            int numCounted = 0;
            PlanStage::StageState countState;

            // Begin running the count
            while (numCounted < 2) {
                countState = count.work(&wsid);
                if (PlanStage::ADVANCED == countState) numCounted++;
            }

            // Prepare the cursor to yield
            count.prepareToYield();

            // Remove remaining objects
            remove(BSON("a" << GTE << 5));

            // Recover from yield
            count.recoverFromYield();

            // finish counting
            while (PlanStage::IS_EOF != countState) {
                countState = count.work(&wsid);
                if (PlanStage::ADVANCED == countState) numCounted++;
            }
            ASSERT_EQUALS(2, numCounted);
        }
    };

    //
    // Check that cursor relocates its end location properly if end location
    // changes during a yield
    //
    class QueryStageCountInsertNewDocsDuringYield : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert documents, add index
            for (int i = 0; i < 10; ++i) {
                insert(BSON("a" << i));
            }
            addIndex(BSON("a" << 1));

            // Set up count stage
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 2);
            params.startKeyInclusive = false;
            params.endKey = BSON("" << 6);
            params.endKeyInclusive = true;

            WorkingSet ws;
            Count count(params, &ws);
            WorkingSetID wsid;

            int numCounted = 0;
            PlanStage::StageState countState;

            // Begin running the count
            while (numCounted < 2) {
                countState = count.work(&wsid);
                if (PlanStage::ADVANCED == countState) numCounted++;
            }

            // Prepare the cursor to yield
            count.prepareToYield();

            // Insert one document before the end
            insert(BSON("a" << 5.5));

            // Insert one document after the end
            insert(BSON("a" << 6.5));

            // Recover from yield
            count.recoverFromYield();

            // finish counting
            while (PlanStage::IS_EOF != countState) {
                countState = count.work(&wsid);
                if (PlanStage::ADVANCED == countState) numCounted++;
            }
            ASSERT_EQUALS(5, numCounted);
        }
    };

    //
    // Check that count performs correctly if an index becomes multikey
    // during a yield
    //
    class QueryStageCountBecomesMultiKeyDuringYield : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert documents, add index
            for (int i = 0; i < 10; ++i) {
                insert(BSON("a" << i));
            }
            addIndex(BSON("a" << 1));

            // Set up count stage
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 2);
            params.startKeyInclusive = false;
            params.endKey = BSON("" << 50);
            params.endKeyInclusive = true;

            WorkingSet ws;
            Count count(params, &ws);
            WorkingSetID wsid;

            int numCounted = 0;
            PlanStage::StageState countState;

            // Begin running the count
            while (numCounted < 2) {
                countState = count.work(&wsid);
                if (PlanStage::ADVANCED == countState) numCounted++;
            }

            // Prepare the cursor to yield
            count.prepareToYield();

            // Insert a document with two values for 'a'
            insert(BSON("a" << BSON_ARRAY(10 << 11)));

            // Recover from yield
            count.recoverFromYield();

            // finish counting
            while (PlanStage::IS_EOF != countState) {
                countState = count.work(&wsid);
                if (PlanStage::ADVANCED == countState) numCounted++;
            }
            ASSERT_EQUALS(8, numCounted);
        }
    };

    //
    // Unused keys are not returned during iteration
    //
    class QueryStageCountUnusedKeys : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert docs, add index
            for (int i = 0; i < 10; ++i) {
                insert(BSON("a" << 1 << "b" << i));
            }
            addIndex(BSON("a" << 1));

            // Mark several keys as 'unused'
            remove(BSON("a" << 1 << "b" << 0));
            remove(BSON("a" << 1 << "b" << 3));
            remove(BSON("a" << 1 << "b" << 4));

            // Ensure that count does not include unused keys
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 1);
            params.startKeyInclusive = true;
            params.endKey = BSON("" << 1);
            params.endKeyInclusive = true;

            WorkingSet ws;
            Count count(params, &ws);

            int numCounted = runCount(&count);
            ASSERT_EQUALS(7, numCounted);
        }
    };

    //
    // Iteration is properly terminated when the end location is an unused key
    //
    class QueryStageCountUnusedEndKey : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert docs, add index
            for (int i = 0; i < 10; ++i) {
                insert(BSON("a" << 1 << "b" << i ));
            }
            addIndex(BSON("a" << 1));

            // Mark key at end position as 'unused' by deleting
            remove(BSON("a" << 1 << "b" << 9));

            // Run count and check
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 0);
            params.startKeyInclusive = true;
            params.endKey = BSON("" << 2);
            params.endKeyInclusive = true; // yes?

            WorkingSet ws;
            Count count(params, &ws);

            int numCounted = runCount(&count);
            ASSERT_EQUALS(9, numCounted);
        }
    };

    //
    // Advances past a key that becomes unused during a yield
    //
    class QueryStageCountKeyBecomesUnusedDuringYield : public CountBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            // Insert documents, add index
            for (int i = 0; i < 10; ++i) {
                insert(BSON("a" << 1 << "b" << i));
            }
            addIndex(BSON("a" << 1));

            // Set up count stage
            CountParams params;
            params.descriptor = getIndex(ctx.ctx().db(), BSON("a" << 1));
            params.startKey = BSON("" << 1);
            params.startKeyInclusive = true;
            params.endKey = BSON("" << 1);
            params.endKeyInclusive = true;

            WorkingSet ws;
            Count count(params, &ws);
            WorkingSetID wsid;

            int numCounted = 0;
            PlanStage::StageState countState;

            // Begin running the count
            while (numCounted < 2) {
                countState = count.work(&wsid);
                if (PlanStage::ADVANCED == countState) numCounted++;
            }

            // Prepare the cursor to yield
            count.prepareToYield();

            // Mark the key at position 5 as 'unused'
            remove(BSON("a" << 1 << "b" << 5));

            // Recover from yield
            count.recoverFromYield();

            // finish counting
            while (PlanStage::IS_EOF != countState) {
                countState = count.work(&wsid);
                if (PlanStage::ADVANCED == countState) numCounted++;
            }
            ASSERT_EQUALS(8, numCounted);
        }
    };

    class All : public Suite {
    public:
        All() : Suite("query_stage_count") { }

        void setupTests() {
            add<QueryStageCountDups>();
            add<QueryStageCountInclusiveBounds>();
            add<QueryStageCountExclusiveBounds>();
            add<QueryStageCountLowerBound>();
            add<QueryStageCountNothingInInterval>();
            add<QueryStageCountNothingInIntervalFirstMatchTooHigh>();
            add<QueryStageCountNoChangeDuringYield>();
            add<QueryStageCountDeleteDuringYield>();
            add<QueryStageCountInsertNewDocsDuringYield>();
            add<QueryStageCountBecomesMultiKeyDuringYield>();
            add<QueryStageCountUnusedKeys>();
        }
    }  queryStageCountAll;

} // namespace QueryStageCount


