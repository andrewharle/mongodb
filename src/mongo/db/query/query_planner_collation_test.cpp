/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_planner_test_fixture.h"

namespace {

using namespace mongo;

TEST_F(QueryPlannerTest, StringComparisonWithNullCollatorOnIndexResultsInCollscan) {
    addIndex(fromjson("{a: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 'foo'}}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, StringComparisonWithNullCollatorOnQueryResultsInCollscan) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    addIndex(fromjson("{a: 1}"), &collator);

    runQuery(fromjson("{a: {$lt: 'foo'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, StringComparisonWithUnequalCollatorsResultsInCollscan) {
    CollatorInterfaceMock alwaysEqualCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    addIndex(fromjson("{a: 1}"), &alwaysEqualCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 'foo'}}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, StringComparisonWithMatchingCollationUsesIndexWithTransformedBounds) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 'foo'}}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, collation: {locale: 'reverse'}, node: {ixscan: "
        "{pattern: {a: 1}, filter: null, "
        "bounds: {a: [['', 'oof', true, false]]}}}}}");
}

TEST_F(QueryPlannerTest, StringComparisonAndNonStringComparisonCanUseSeparateIndices) {
    CollatorInterfaceMock reverseStringCollator(CollatorInterfaceMock::MockType::kReverseString);
    CollatorInterfaceMock alwaysEqualCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);

    addIndex(fromjson("{a: 1}"), &reverseStringCollator);
    addIndex(fromjson("{b: 1}"), &alwaysEqualCollator);

    // The string predicate can use index {a: 1}, since the collators match. The non-string
    // comparison can use index {b: 1}, even though the collators don't match.
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$lt: 'foo'}, b: {$lte: 4}}, collation: {locale: "
                 "'reverse'}}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {b: {$lte: 4}}, collation: {locale: 'reverse'}, node: "
        "{ixscan: {pattern: {a: 1}, "
        "filter: null, bounds: {a: [['', 'oof', true, false]]}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a: {$lt: 'foo'}}, collation: {locale: 'reverse'}, node: {ixscan: "
        "{pattern: {b: 1}, filter: null, "
        "bounds: {b: [[-Infinity, 4, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, StringEqWrtCollatorCannotBeCovered) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 'string'}, projection: {_id: 0, a: 1}, collation: "
                 "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: "
        "{ixscan: {pattern: {a: 1}, filter: null, bounds: {a: [['gnirts', 'gnirts', true, "
        "true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, StringGteWrtCollatorCannotBeCovered) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$gte: 'string'}}, projection: {_id: 0, a: 1}, collation: "
        "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: "
        "{ixscan: {pattern: {a: 1}, filter: null, bounds: {a: [['gnirts', {}, true, "
        "false]]}}}}}}}");
}

TEST_F(QueryPlannerTest, InContainingStringCannotBeCoveredWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$in: [2, 'foo']}}, projection: {_id: 0, a: 1}, collation: "
        "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: "
        "{ixscan: {pattern: {a: 1}, filter: null, bounds: {a: [[2,2,true,true],"
        "['oof','oof',true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, TypeStringCannotBeCoveredWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$type: 'string'}}, projection: {_id: 0, a: 1}, collation: "
        "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: {a:{$type:'string'}}, collation: "
        "{locale: 'reverse'}, node: {ixscan: {pattern: {a: 1}, filter: null, "
        "bounds: {a: [['',{},true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, NotWithStringBoundsCannotBeCoveredWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {$ne: 2}}, projection: {_id: 0, a: 1}, collation: "
                 "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: {ixscan: {pattern: {a: 1}, filter: null, "
        "bounds: {a: [['MinKey',2,true,false], [2,'MaxKey',false,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, ExistsTrueCannotBeCoveredWithSparseIndexAndCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);
    params.indices.back().sparse = true;

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$exists: true}}, projection: {_id: 0, a: 1}, collation: "
        "{locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{proj: {spec: {_id: 0, a: 1}, node: {cscan: {dir: 1}}}}");
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: {ixscan: {pattern: {a: 1}, filter: null, "
        "bounds: {a: [['MinKey','MaxKey',true,true]]}}}}}}}");
}

TEST_F(QueryPlannerTest, MinMaxWithStringBoundsCannotBeCoveredWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: 2}, max: {a: 2, b: 1}, "
                 "projection: {_id: 0, a: 1, b: 1}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: {fetch: {filter: null, collation: "
        "{locale: 'reverse'}, node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, MinMaxWithoutStringBoundsBoundsCanBeCoveredWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: 2}, max: {a: 1, b: 2}, "
                 "projection: {_id: 0, a: 1, b: 1}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{proj: {spec: {_id: 0, a: 1, b: 1}, node: {ixscan: {pattern: {a: 1, b: 1}}}}}");
}

TEST_F(QueryPlannerTest, SimpleRegexCanUseAnIndexWithACollatorWithLooseBounds) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    // Since the index has a collation, the regex must be applied after fetching the documents
    // (INEXACT_FETCH tightness).
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: /^simple/}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a: /^simple/}, node: {ixscan: {pattern: {a: 1}, filter: null, bounds: "
        "{a: [['', {}, true, false], [/^simple/, /^simple/, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, SimpleRegexCanUseAnIndexWithoutACollatorWithTightBounds) {
    addIndex(fromjson("{a: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: /^simple/}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: 1}, filter: null, bounds: "
        "{a: [['simple', 'simplf', true, false], [/^simple/, /^simple/, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, NonSimpleRegexCanUseAnIndexWithoutACollatorAsInexactCovered) {
    addIndex(fromjson("{a: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: /nonsimple/}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: 1}, filter: {a: /nonsimple/}, bounds: "
        "{a: [['', {}, true, false], [/nonsimple/, /nonsimple/, true, true]]}}}}}");
}

TEST_F(QueryPlannerTest, AccessPlannerCorrectlyCombinesComparisonKeyBounds) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1, b: 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$gte: 'foo', $lte: 'zfoo'}, b: 'bar'}, collation: {locale: "
        "'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: null, collation: {locale: 'reverse'}, "
        "node: {ixscan: {pattern: {a: 1, b: "
        "1}, filter: null, bounds: {a: [['oof','oofz',true,true]], b: "
        "[['rab','rab',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, OrQueryResultsInCollscanWhenOnlyOneBranchHasIndexWithMatchingCollation) {
    CollatorInterfaceMock reverseStringCollator(CollatorInterfaceMock::MockType::kReverseString);
    CollatorInterfaceMock alwaysEqualCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);

    addIndex(fromjson("{a: 1}"), &reverseStringCollator);
    addIndex(fromjson("{b: 1}"), &alwaysEqualCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {$or: [{a: 'foo'}, {b: 'bar'}]}, collation: {locale: "
                 "'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, OrQueryCanBeIndexedWhenBothBranchesHaveIndexWithMatchingCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);
    addIndex(fromjson("{b: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {$or: [{a: 'foo'}, {b: 'bar'}]}, collation: {locale: "
                 "'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {or: {nodes: ["
        "{ixscan: {pattern: {a: 1}, bounds: {a: [['oof','oof',true,true]]}}},"
        "{ixscan: {pattern: {b: 1}, bounds: {b: [['rab','rab',true,true]]}}}]}}}}");
}

TEST_F(QueryPlannerTest, ElemMatchObjectResultsInCorrectComparisonKeyBounds) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{'a.b': 1}"), &collator);

    runQueryAsCommand(fromjson(
        "{find: 'testns', filter: {a: {$elemMatch: {b: {$gte: 'foo', $lte: 'zfoo'}}}}, collation: "
        "{locale: "
        "'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$elemMatch:{b:{$gte:'foo',$lte:'zfoo'}}}}, collation: {locale: "
        "'reverse'}, node: {ixscan: {pattern: "
        "{'a.b': 1}, filter: null, bounds: {'a.b': [['oof','oofz',true,true]]}}}}}");
}

TEST_F(QueryPlannerTest, QueryForNestedObjectWithMatchingCollatorCanUseIndex) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {b: 1}}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}}}}}");
}

TEST_F(QueryPlannerTest, QueryForNestedObjectWithNonMatchingCollatorCantUseIndexWithCollator) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator);

    runQuery(fromjson("{a: {b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, CannotUseIndexWithNonMatchingCollatorForSort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator);

    runQueryAsCommand(fromjson("{find: 'testns', filter: {b: 1}, sort: {a: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen:"
        "{node: {cscan: {dir: 1, filter: {b: 1}}}}}}}");
}

TEST_F(QueryPlannerTest, CanUseIndexWithMatchingCollatorForSort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {b: 1}, sort: {a: 1}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen:"
        "{node: {cscan: {dir: 1, filter: {b: 1}, collation: {locale: 'reverse'}}}}}}}");
    assertSolutionExists(
        "{fetch: {filter: {b: 1}, collation: {locale: 'reverse'}, node: {ixscan: {pattern: {a: "
        "1}}}}}");
}

TEST_F(QueryPlannerTest, IndexWithNonMatchingCollatorCausesInMemorySort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator);

    runQueryAsCommand(fromjson("{find: 'testns', filter: {a: {'$exists': true}}, sort: {a: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen:"
        "{node: {fetch: {node : {ixscan: {pattern: {a: 1}}}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen:"
        "{node: {cscan: {dir: 1, filter: {a: {'$exists': true}}}}}}}}");
}

TEST_F(QueryPlannerTest, IndexWithMatchingCollatorDoesNotCauseInMemorySort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: {'$exists': true}}, sort: {a: 1},"
                 "collation: {locale: 'reverse'}}"));

    assertNumSolutions(2U);
    assertSolutionExists("{fetch: {node : {ixscan: {pattern: {a: 1}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {a: 1}, limit: 0, node: {sortKeyGen:"
        "{node: {cscan: {dir: 1, filter: {a: {'$exists': true}}}}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexWithNonMatchingCollatorCausesInMemorySort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1, b: 1, c: 1, d: 1}"), &indexCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 1, b: 2, c: {a: 1}},"
                 "sort: {a: 1, b: 1, c: 1}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1, c: 1}, limit: 0, node: {sortKeyGen:"
        "{node: {fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c: 1, d: 1}}}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1, c: 1}, limit: 0, node: {sortKeyGen:"
        "{node: {cscan: {dir: 1, filter: {a: 1, b: 2, c: {a: 1}}}}}}}}");
}

TEST_F(QueryPlannerTest, CompoundIndexWithNonMatchingPrefixedCollatorDoesNotCauseInMemorySort) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1, b: 1, c: 1, d: 1}"), &indexCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {a: 1, b: 2, c: {a: 1 } },"
                 "sort: {a: 1, b: 1 }}"));

    assertNumSolutions(2U);
    assertSolutionExists("{fetch: {node : {ixscan: {pattern: {a: 1, b: 1, c: 1, d: 1}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, node: {sortKeyGen:"
        "{node: {cscan: {dir: 1, filter : {a: 1, b: 2, c: {a: 1}}}}}}}}");
}

TEST_F(QueryPlannerTest, SuccessfullyPlanWhenMinMaxHaveNumberBoundariesAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);

    runQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: 1, c: 1}, max: {a: 3, b: 3, c: 3}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c: 1}}}}}");
}

TEST_F(QueryPlannerTest, FailToPlanWhenMinHasStringBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(fromjson("{find: 'testns', min: {a: 1, b: 'foo', c: 1}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenMaxHasStringBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(fromjson("{find: 'testns', max: {a: 1, b: 'foo', c: 1}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenMinHasObjectBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(fromjson("{find: 'testns', min: {a: 1, b: {d: 'foo'}, c: 1}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenMaxHasObjectBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(fromjson("{find: 'testns', max: {a: 1, b: {d: 'foo'}, c: 1}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenMinHasArrayBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(fromjson("{find: 'testns', min: {a: 1, b: 1, c: [1, 'foo']}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenMaxHasArrayBoundaryAndCollationsDontMatch) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1, b: 1, c: 1}"), &indexCollator);
    runInvalidQueryAsCommand(fromjson("{find: 'testns', max: {a: 1, b: 1, c: [1, 'foo']}}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenHintingIndexIncompatibleWithMinDueToCollation) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1}"), &indexCollator, "indexToHint"_sd);
    addIndex(fromjson("{a: 1}"));
    runInvalidQueryAsCommand(fromjson("{find: 'testns', min: {a: 'foo'}, hint: 'indexToHint'}"));
}

TEST_F(QueryPlannerTest, FailToPlanWhenHintingIndexIncompatibleWithMaxDueToCollation) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1}"), &indexCollator, "indexToHint"_sd);
    addIndex(fromjson("{a: 1}"));
    runInvalidQueryAsCommand(fromjson("{find: 'testns', max: {a: 'foo'}, hint: 'indexToHint'}"));
}

TEST_F(QueryPlannerTest, SelectIndexWithMatchingSimpleCollationWhenMinHasStringBoundary) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1}"), &indexCollator, "withCollation"_sd);
    addIndex(fromjson("{a: 1}"), nullptr, "noCollation"_sd);

    runQueryAsCommand(fromjson("{find: 'testns', min: {a: 'foo'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}, name: 'noCollation'}}}}");
}

TEST_F(QueryPlannerTest, SelectIndexWithMatchingNonSimpleCollationWhenMinHasStringBoundary) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &indexCollator, "withCollation"_sd);
    addIndex(fromjson("{a: 1}"), nullptr, "noCollation"_sd);

    runQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 'foo'}, collation: {locale: 'reverse'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}, name: 'withCollation'}}}}");
}

TEST_F(QueryPlannerTest, SelectIndexWithMatchingSimpleCollationWhenMaxHasStringBoundary) {
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kToLowerString);
    addIndex(fromjson("{a: 1}"), &indexCollator, "withCollation"_sd);
    addIndex(fromjson("{a: 1}"), nullptr, "noCollation"_sd);

    runQueryAsCommand(fromjson("{find: 'testns', max: {a: 'foo'}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1}, name: 'noCollation'}}}}");
}

TEST_F(QueryPlannerTest, MustSortInMemoryWhenMinMaxIndexCollationDoesNotMatch) {
    addIndex(fromjson("{a: 1, b: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: 1}, max: {a: 2, b: 1}, collation: {locale: "
                 "'reverse'}, sort: {a: 1, b: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{sort: {pattern: {a: 1, b: 1}, limit: 0, node: {sortKeyGen:"
        "{node: {fetch: {node: {ixscan: {pattern: {a: 1, b: 1}}}}}}}}}");
}

TEST_F(QueryPlannerTest, NoSortStageWhenMinMaxIndexCollationDoesNotMatchButBoundsContainNoStrings) {
    addIndex(fromjson("{a: 1, b: 1, c: 1}"));

    runQueryAsCommand(
        fromjson("{find: 'testns', min: {a: 1, b: 8, c: 1}, max: {a: 1, b: 8, c: 100}, collation: "
                 "{locale: 'reverse'}, sort: {a: 1, b: 1, c: 1}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: 1, b: 1, c: 1}}}}}");
}

}  // namespace
