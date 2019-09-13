
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

/** Unit tests for MatchMatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/unittest/unittest.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"

namespace mongo {

using std::string;

TEST(ComparisonMatchExpression, ComparisonMatchExpressionsWithUnequalCollatorsAreUnequal) {
    BSONObj operand = BSON("a" << 5);
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kReverseString);
    EqualityMatchExpression eq1("a", operand["a"]);
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    EqualityMatchExpression eq2("a", operand["a"]);
    eq2.setCollator(&collator2);
    ASSERT(!eq1.equivalent(&eq2));
}

TEST(ComparisonMatchExpression, ComparisonMatchExpressionsWithEqualCollatorsAreEqual) {
    BSONObj operand = BSON("a" << 5);
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kAlwaysEqual);
    EqualityMatchExpression eq1("a", operand["a"]);
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    EqualityMatchExpression eq2("a", operand["a"]);
    eq2.setCollator(&collator2);
    ASSERT(eq1.equivalent(&eq2));
}

TEST(ComparisonMatchExpression, StringMatchingWithNullCollatorUsesBinaryComparison) {
    BSONObj operand = BSON("a"
                           << "string");
    EqualityMatchExpression eq("a", operand["a"]);
    ASSERT(!eq.matchesBSON(BSON("a"
                                << "string2"),
                           NULL));
}

TEST(ComparisonMatchExpression, StringMatchingRespectsCollation) {
    BSONObj operand = BSON("a"
                           << "string");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    EqualityMatchExpression eq("a", operand["a"]);
    eq.setCollator(&collator);
    ASSERT(eq.matchesBSON(BSON("a"
                               << "string2"),
                          NULL));
}

TEST(EqOp, MatchesElement) {
    BSONObj operand = BSON("a" << 5);
    BSONObj match = BSON("a" << 5.0);
    BSONObj notMatch = BSON("a" << 6);

    EqualityMatchExpression eq("", operand["a"]);
    ASSERT(eq.matchesSingleElement(match.firstElement()));
    ASSERT(!eq.matchesSingleElement(notMatch.firstElement()));

    ASSERT(eq.equivalent(&eq));
}

DEATH_TEST(EqOp, InvalidEooOperand, "Invariant failure _rhs") {
    BSONObj operand;
    EqualityMatchExpression eq("", operand.firstElement());
}

TEST(EqOp, MatchesScalar) {
    BSONObj operand = BSON("a" << 5);
    EqualityMatchExpression eq("a", operand["a"]);
    ASSERT(eq.matchesBSON(BSON("a" << 5.0), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << 4), NULL));
}

TEST(EqOp, MatchesArrayValue) {
    BSONObj operand = BSON("a" << 5);
    EqualityMatchExpression eq("a", operand["a"]);
    ASSERT(eq.matchesBSON(BSON("a" << BSON_ARRAY(5.0 << 6)), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << BSON_ARRAY(6 << 7)), NULL));
}

TEST(EqOp, MatchesReferencedObjectValue) {
    BSONObj operand = BSON("a.b" << 5);
    EqualityMatchExpression eq("a.b", operand["a.b"]);
    ASSERT(eq.matchesBSON(BSON("a" << BSON("b" << 5)), NULL));
    ASSERT(eq.matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY(5))), NULL));
    ASSERT(eq.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 5))), NULL));
}

TEST(EqOp, MatchesReferencedArrayValue) {
    BSONObj operand = BSON("a.0" << 5);
    EqualityMatchExpression eq("a.0", operand["a.0"]);
    ASSERT(eq.matchesBSON(BSON("a" << BSON_ARRAY(5)), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), NULL));
}

TEST(EqOp, MatchesNull) {
    BSONObj operand = BSON("a" << BSONNULL);
    EqualityMatchExpression eq("a", operand["a"]);
    ASSERT(eq.matchesBSON(BSONObj(), NULL));
    ASSERT(eq.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << 4), NULL));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(eq.matchesBSON(BSON("b" << 4), NULL));
}

// This test documents how the matcher currently works,
// not necessarily how it should work ideally.
TEST(EqOp, MatchesNestedNull) {
    BSONObj operand = BSON("a.b" << BSONNULL);
    EqualityMatchExpression eq("a.b", operand["a.b"]);
    // null matches any empty object that is on a subpath of a.b
    ASSERT(eq.matchesBSON(BSONObj(), NULL));
    ASSERT(eq.matchesBSON(BSON("a" << BSONObj()), NULL));
    ASSERT(eq.matchesBSON(BSON("a" << BSON_ARRAY(BSONObj())), NULL));
    ASSERT(eq.matchesBSON(BSON("a" << BSON("b" << BSONNULL)), NULL));
    // b does not exist as an element in array under a.
    ASSERT(!eq.matchesBSON(BSON("a" << BSONArray()), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << BSON_ARRAY(BSONNULL)), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2)), NULL));
    // a.b exists but is not null.
    ASSERT(!eq.matchesBSON(BSON("a" << BSON("b" << 4)), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << BSON("b" << BSONObj())), NULL));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(eq.matchesBSON(BSON("b" << 4), NULL));
}

TEST(EqOp, MatchesMinKey) {
    BSONObj operand = BSON("a" << MinKey);
    EqualityMatchExpression eq("a", operand["a"]);
    ASSERT(eq.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << 4), NULL));
}


TEST(EqOp, MatchesMaxKey) {
    BSONObj operand = BSON("a" << MaxKey);
    EqualityMatchExpression eq("a", operand["a"]);
    ASSERT(eq.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << 4), NULL));
}

TEST(EqOp, MatchesFullArray) {
    BSONObj operand = BSON("a" << BSON_ARRAY(1 << 2));
    EqualityMatchExpression eq("a", operand["a"]);
    ASSERT(eq.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2)), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 3)), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << BSON_ARRAY(1)), NULL));
    ASSERT(!eq.matchesBSON(BSON("a" << 1), NULL));
}

TEST(EqOp, MatchesThroughNestedArray) {
    BSONObj operand = BSON("a.b.c.d" << 3);
    EqualityMatchExpression eq("a.b.c.d", operand["a.b.c.d"]);
    BSONObj obj = fromjson("{a:{b:[{c:[{d:1},{d:2}]},{c:[{d:3}]}]}}");
    ASSERT(eq.matchesBSON(obj, NULL));
}

TEST(EqOp, ElemMatchKey) {
    BSONObj operand = BSON("a" << 5);
    EqualityMatchExpression eq("a", operand["a"]);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!eq.matchesBSON(BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(eq.matchesBSON(BSON("a" << 5), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(eq.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("2", details.elemMatchKey());
}

// SERVER-14886: when an array is being traversed explictly at the same time that a nested array
// is being traversed implicitly, the elemMatch key should refer to the offset of the array
// being implicitly traversed.
TEST(EqOp, ElemMatchKeyWithImplicitAndExplicitTraversal) {
    BSONObj operand = BSON("a.0.b" << 3);
    BSONElement operandFirstElt = operand.firstElement();
    EqualityMatchExpression eq(operandFirstElt.fieldName(), operandFirstElt);
    MatchDetails details;
    details.requestElemMatchKey();
    BSONObj obj = fromjson("{a: [{b: [2, 3]}, {b: [4, 5]}]}");
    ASSERT(eq.matchesBSON(obj, &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(EqOp, Equality1) {
    BSONObj operand = BSON("a" << 5 << "b" << 5 << "c" << 4);
    EqualityMatchExpression eq1("a", operand["a"]);
    EqualityMatchExpression eq2("a", operand["b"]);
    EqualityMatchExpression eq3("c", operand["c"]);

    ASSERT(eq1.equivalent(&eq1));
    ASSERT(eq1.equivalent(&eq2));
    ASSERT(!eq1.equivalent(&eq3));
}

TEST(LtOp, MatchesElement) {
    BSONObj operand = BSON("$lt" << 5);
    BSONObj match = BSON("a" << 4.5);
    BSONObj notMatch = BSON("a" << 6);
    BSONObj notMatchEqual = BSON("a" << 5);
    BSONObj notMatchWrongType = BSON("a"
                                     << "foo");
    LTMatchExpression lt("", operand["$lt"]);
    ASSERT(lt.matchesSingleElement(match.firstElement()));
    ASSERT(!lt.matchesSingleElement(notMatch.firstElement()));
    ASSERT(!lt.matchesSingleElement(notMatchEqual.firstElement()));
    ASSERT(!lt.matchesSingleElement(notMatchWrongType.firstElement()));
}

DEATH_TEST(LtOp, InvalidEooOperand, "Invariant failure _rhs") {
    BSONObj operand;
    LTMatchExpression lt("", operand.firstElement());
}

TEST(LtOp, MatchesScalar) {
    BSONObj operand = BSON("$lt" << 5);
    LTMatchExpression lt("a", operand["$lt"]);
    ASSERT(lt.matchesBSON(BSON("a" << 4.5), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << 6), NULL));
}

TEST(LtOp, MatchesScalarEmptyKey) {
    BSONObj operand = BSON("$lt" << 5);
    LTMatchExpression lt("", operand["$lt"]);
    ASSERT(lt.matchesBSON(BSON("" << 4.5), NULL));
    ASSERT(!lt.matchesBSON(BSON("" << 6), NULL));
}

TEST(LtOp, MatchesArrayValue) {
    BSONObj operand = BSON("$lt" << 5);
    LTMatchExpression lt("a", operand["$lt"]);
    ASSERT(lt.matchesBSON(BSON("a" << BSON_ARRAY(6 << 4.5)), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSON_ARRAY(6 << 7)), NULL));
}

TEST(LtOp, MatchesWholeArray) {
    BSONObj operand = BSON("$lt" << BSON_ARRAY(5));
    LTMatchExpression lt("a", operand["$lt"]);
    ASSERT(lt.matchesBSON(BSON("a" << BSON_ARRAY(4)), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSON_ARRAY(5)), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSON_ARRAY(6)), NULL));
    // Nested array.
    ASSERT(lt.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(4))), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(6))), NULL));
}

TEST(LtOp, MatchesNull) {
    BSONObj operand = BSON("$lt" << BSONNULL);
    LTMatchExpression lt("a", operand["$lt"]);
    ASSERT(!lt.matchesBSON(BSONObj(), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << 4), NULL));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(!lt.matchesBSON(BSON("b" << 4), NULL));
}

TEST(LtOp, MatchesDotNotationNull) {
    BSONObj operand = BSON("$lt" << BSONNULL);
    LTMatchExpression lt("a.b", operand["$lt"]);
    ASSERT(!lt.matchesBSON(BSONObj(), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << 4), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSONObj()), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << BSONNULL))), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSON_ARRAY(BSON("a" << 4) << BSON("b" << 4))), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSON_ARRAY(4)), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 4))), NULL));
}

TEST(LtOp, MatchesMinKey) {
    BSONObj operand = BSON("a" << MinKey);
    LTMatchExpression lt("a", operand["a"]);
    ASSERT(!lt.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(!lt.matchesBSON(BSON("a" << 4), NULL));
}

TEST(LtOp, MatchesMaxKey) {
    BSONObj operand = BSON("a" << MaxKey);
    LTMatchExpression lt("a", operand["a"]);
    ASSERT(!lt.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(lt.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(lt.matchesBSON(BSON("a" << 4), NULL));
}

TEST(LtOp, ElemMatchKey) {
    BSONObj operand = BSON("$lt" << 5);
    LTMatchExpression lt("a", operand["$lt"]);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!lt.matchesBSON(BSON("a" << 6), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(lt.matchesBSON(BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(lt.matchesBSON(BSON("a" << BSON_ARRAY(6 << 2 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(LteOp, MatchesElement) {
    BSONObj operand = BSON("$lte" << 5);
    BSONObj match = BSON("a" << 4.5);
    BSONObj equalMatch = BSON("a" << 5);
    BSONObj notMatch = BSON("a" << 6);
    BSONObj notMatchWrongType = BSON("a"
                                     << "foo");
    LTEMatchExpression lte("", operand["$lte"]);
    ASSERT(lte.matchesSingleElement(match.firstElement()));
    ASSERT(lte.matchesSingleElement(equalMatch.firstElement()));
    ASSERT(!lte.matchesSingleElement(notMatch.firstElement()));
    ASSERT(!lte.matchesSingleElement(notMatchWrongType.firstElement()));
}

DEATH_TEST(LteOp, InvalidEooOperand, "Invariant failure _rhs") {
    BSONObj operand;
    LTEMatchExpression lte("", operand.firstElement());
}

TEST(LteOp, MatchesScalar) {
    BSONObj operand = BSON("$lte" << 5);
    LTEMatchExpression lte("a", operand["$lte"]);
    ASSERT(lte.matchesBSON(BSON("a" << 4.5), NULL));
    ASSERT(!lte.matchesBSON(BSON("a" << 6), NULL));
}

TEST(LteOp, MatchesArrayValue) {
    BSONObj operand = BSON("$lte" << 5);
    LTEMatchExpression lte("a", operand["$lte"]);
    ASSERT(lte.matchesBSON(BSON("a" << BSON_ARRAY(6 << 4.5)), NULL));
    ASSERT(!lte.matchesBSON(BSON("a" << BSON_ARRAY(6 << 7)), NULL));
}

TEST(LteOp, MatchesWholeArray) {
    BSONObj operand = BSON("$lte" << BSON_ARRAY(5));
    LTEMatchExpression lte("a", operand["$lte"]);
    ASSERT(lte.matchesBSON(BSON("a" << BSON_ARRAY(4)), NULL));
    ASSERT(lte.matchesBSON(BSON("a" << BSON_ARRAY(5)), NULL));
    ASSERT(!lte.matchesBSON(BSON("a" << BSON_ARRAY(6)), NULL));
    // Nested array.
    ASSERT(lte.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(4))), NULL));
    ASSERT(lte.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), NULL));
    ASSERT(!lte.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(6))), NULL));
}

TEST(LteOp, MatchesNull) {
    BSONObj operand = BSON("$lte" << BSONNULL);
    LTEMatchExpression lte("a", operand["$lte"]);
    ASSERT(lte.matchesBSON(BSONObj(), NULL));
    ASSERT(lte.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(!lte.matchesBSON(BSON("a" << 4), NULL));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(lte.matchesBSON(BSON("b" << 4), NULL));
}

TEST(LteOp, MatchesDotNotationNull) {
    BSONObj operand = BSON("$lte" << BSONNULL);
    LTEMatchExpression lte("a.b", operand["$lte"]);
    ASSERT(lte.matchesBSON(BSONObj(), NULL));
    ASSERT(lte.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(lte.matchesBSON(BSON("a" << 4), NULL));
    ASSERT(lte.matchesBSON(BSON("a" << BSONObj()), NULL));
    ASSERT(lte.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << BSONNULL))), NULL));
    ASSERT(lte.matchesBSON(BSON("a" << BSON_ARRAY(BSON("a" << 4) << BSON("b" << 4))), NULL));
    ASSERT(!lte.matchesBSON(BSON("a" << BSON_ARRAY(4)), NULL));
    ASSERT(!lte.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 4))), NULL));
}

TEST(LteOp, MatchesMinKey) {
    BSONObj operand = BSON("a" << MinKey);
    LTEMatchExpression lte("a", operand["a"]);
    ASSERT(lte.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(!lte.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(!lte.matchesBSON(BSON("a" << 4), NULL));
}

TEST(LteOp, MatchesMaxKey) {
    BSONObj operand = BSON("a" << MaxKey);
    LTEMatchExpression lte("a", operand["a"]);
    ASSERT(lte.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(lte.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(lte.matchesBSON(BSON("a" << 4), NULL));
}


TEST(LteOp, ElemMatchKey) {
    BSONObj operand = BSON("$lte" << 5);
    LTEMatchExpression lte("a", operand["$lte"]);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!lte.matchesBSON(BSON("a" << 6), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(lte.matchesBSON(BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(lte.matchesBSON(BSON("a" << BSON_ARRAY(6 << 2 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

DEATH_TEST(GtOp, InvalidEooOperand, "Invariant failure _rhs") {
    BSONObj operand;
    GTMatchExpression gt("", operand.firstElement());
}

TEST(GtOp, MatchesScalar) {
    BSONObj operand = BSON("$gt" << 5);
    GTMatchExpression gt("a", operand["$gt"]);
    ASSERT(gt.matchesBSON(BSON("a" << 5.5), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << 4), NULL));
}

TEST(GtOp, MatchesArrayValue) {
    BSONObj operand = BSON("$gt" << 5);
    GTMatchExpression gt("a", operand["$gt"]);
    ASSERT(gt.matchesBSON(BSON("a" << BSON_ARRAY(3 << 5.5)), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << BSON_ARRAY(2 << 4)), NULL));
}

TEST(GtOp, MatchesWholeArray) {
    BSONObj operand = BSON("$gt" << BSON_ARRAY(5));
    GTMatchExpression gt("a", operand["$gt"]);
    ASSERT(!gt.matchesBSON(BSON("a" << BSON_ARRAY(4)), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << BSON_ARRAY(5)), NULL));
    ASSERT(gt.matchesBSON(BSON("a" << BSON_ARRAY(6)), NULL));
    // Nested array.
    // XXX: The following assertion documents current behavior.
    ASSERT(gt.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(4))), NULL));
    // XXX: The following assertion documents current behavior.
    ASSERT(gt.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), NULL));
    ASSERT(gt.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(6))), NULL));
}

TEST(GtOp, MatchesNull) {
    BSONObj operand = BSON("$gt" << BSONNULL);
    GTMatchExpression gt("a", operand["$gt"]);
    ASSERT(!gt.matchesBSON(BSONObj(), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << 4), NULL));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(!gt.matchesBSON(BSON("b" << 4), NULL));
}

TEST(GtOp, MatchesDotNotationNull) {
    BSONObj operand = BSON("$gt" << BSONNULL);
    GTMatchExpression gt("a.b", operand["$gt"]);
    ASSERT(!gt.matchesBSON(BSONObj(), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << 4), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << BSONObj()), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << BSONNULL))), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << BSON_ARRAY(BSON("a" << 4) << BSON("b" << 4))), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << BSON_ARRAY(4)), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 4))), NULL));
}

TEST(GtOp, MatchesMinKey) {
    BSONObj operand = BSON("a" << MinKey);
    GTMatchExpression gt("a", operand["a"]);
    ASSERT(!gt.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(gt.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(gt.matchesBSON(BSON("a" << 4), NULL));
}

TEST(GtOp, MatchesMaxKey) {
    BSONObj operand = BSON("a" << MaxKey);
    GTMatchExpression gt("a", operand["a"]);
    ASSERT(!gt.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(!gt.matchesBSON(BSON("a" << 4), NULL));
}

TEST(GtOp, ElemMatchKey) {
    BSONObj operand = BSON("$gt" << 5);
    GTMatchExpression gt("a", operand["$gt"]);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!gt.matchesBSON(BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(gt.matchesBSON(BSON("a" << 6), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(gt.matchesBSON(BSON("a" << BSON_ARRAY(2 << 6 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(GteOp, MatchesElement) {
    BSONObj operand = BSON("$gte" << 5);
    BSONObj match = BSON("a" << 5.5);
    BSONObj equalMatch = BSON("a" << 5);
    BSONObj notMatch = BSON("a" << 4);
    BSONObj notMatchWrongType = BSON("a"
                                     << "foo");
    GTEMatchExpression gte("", operand["$gte"]);
    ASSERT(gte.matchesSingleElement(match.firstElement()));
    ASSERT(gte.matchesSingleElement(equalMatch.firstElement()));
    ASSERT(!gte.matchesSingleElement(notMatch.firstElement()));
    ASSERT(!gte.matchesSingleElement(notMatchWrongType.firstElement()));
}

DEATH_TEST(GteOp, InvalidEooOperand, "Invariant failure _rhs") {
    BSONObj operand;
    GTEMatchExpression gte("", operand.firstElement());
}

TEST(GteOp, MatchesScalar) {
    BSONObj operand = BSON("$gte" << 5);
    GTEMatchExpression gte("a", operand["$gte"]);
    ASSERT(gte.matchesBSON(BSON("a" << 5.5), NULL));
    ASSERT(!gte.matchesBSON(BSON("a" << 4), NULL));
}

TEST(GteOp, MatchesArrayValue) {
    BSONObj operand = BSON("$gte" << 5);
    GTEMatchExpression gte("a", operand["$gte"]);
    ASSERT(gte.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5.5)), NULL));
    ASSERT(!gte.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2)), NULL));
}

TEST(GteOp, MatchesWholeArray) {
    BSONObj operand = BSON("$gte" << BSON_ARRAY(5));
    GTEMatchExpression gte("a", operand["$gte"]);
    ASSERT(!gte.matchesBSON(BSON("a" << BSON_ARRAY(4)), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << BSON_ARRAY(5)), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << BSON_ARRAY(6)), NULL));
    // Nested array.
    // XXX: The following assertion documents current behavior.
    ASSERT(gte.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(4))), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(6))), NULL));
}

TEST(GteOp, MatchesNull) {
    BSONObj operand = BSON("$gte" << BSONNULL);
    GTEMatchExpression gte("a", operand["$gte"]);
    ASSERT(gte.matchesBSON(BSONObj(), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(!gte.matchesBSON(BSON("a" << 4), NULL));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(gte.matchesBSON(BSON("b" << 4), NULL));
}

TEST(GteOp, MatchesDotNotationNull) {
    BSONObj operand = BSON("$gte" << BSONNULL);
    GTEMatchExpression gte("a.b", operand["$gte"]);
    ASSERT(gte.matchesBSON(BSONObj(), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << 4), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << BSONObj()), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << BSONNULL))), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << BSON_ARRAY(BSON("a" << 4) << BSON("b" << 4))), NULL));
    ASSERT(!gte.matchesBSON(BSON("a" << BSON_ARRAY(4)), NULL));
    ASSERT(!gte.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 4))), NULL));
}

TEST(GteOp, MatchesMinKey) {
    BSONObj operand = BSON("a" << MinKey);
    GTEMatchExpression gte("a", operand["a"]);
    ASSERT(gte.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(gte.matchesBSON(BSON("a" << 4), NULL));
}

TEST(GteOp, MatchesMaxKey) {
    BSONObj operand = BSON("a" << MaxKey);
    GTEMatchExpression gte("a", operand["a"]);
    ASSERT(gte.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(!gte.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(!gte.matchesBSON(BSON("a" << 4), NULL));
}

TEST(GteOp, ElemMatchKey) {
    BSONObj operand = BSON("$gte" << 5);
    GTEMatchExpression gte("a", operand["$gte"]);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!gte.matchesBSON(BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(gte.matchesBSON(BSON("a" << 6), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(gte.matchesBSON(BSON("a" << BSON_ARRAY(2 << 6 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(RegexMatchExpression, MatchesElementExact) {
    BSONObj match = BSON("a"
                         << "b");
    BSONObj notMatch = BSON("a"
                            << "c");
    RegexMatchExpression regex("", "b", "");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatch.firstElement()));
}

TEST(RegexMatchExpression, TooLargePattern) {
    string tooLargePattern(50 * 1000, 'z');
    ASSERT_THROWS_CODE(RegexMatchExpression regex("a", tooLargePattern, ""),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST(RegexMatchExpression, MatchesElementSimplePrefix) {
    BSONObj match = BSON("x"
                         << "abc");
    BSONObj notMatch = BSON("x"
                            << "adz");
    RegexMatchExpression regex("", "^ab", "");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementCaseSensitive) {
    BSONObj match = BSON("x"
                         << "abc");
    BSONObj notMatch = BSON("x"
                            << "ABC");
    RegexMatchExpression regex("", "abc", "");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementCaseInsensitive) {
    BSONObj match = BSON("x"
                         << "abc");
    BSONObj matchUppercase = BSON("x"
                                  << "ABC");
    BSONObj notMatch = BSON("x"
                            << "abz");
    RegexMatchExpression regex("", "abc", "i");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(regex.matchesSingleElement(matchUppercase.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementMultilineOff) {
    BSONObj match = BSON("x"
                         << "az");
    BSONObj notMatch = BSON("x"
                            << "\naz");
    RegexMatchExpression regex("", "^a", "");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementMultilineOn) {
    BSONObj match = BSON("x"
                         << "az");
    BSONObj matchMultiline = BSON("x"
                                  << "\naz");
    BSONObj notMatch = BSON("x"
                            << "\n\n");
    RegexMatchExpression regex("", "^a", "m");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(regex.matchesSingleElement(matchMultiline.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementExtendedOff) {
    BSONObj match = BSON("x"
                         << "a b");
    BSONObj notMatch = BSON("x"
                            << "ab");
    RegexMatchExpression regex("", "a b", "");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementExtendedOn) {
    BSONObj match = BSON("x"
                         << "ab");
    BSONObj notMatch = BSON("x"
                            << "a b");
    RegexMatchExpression regex("", "a b", "x");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementDotAllOff) {
    BSONObj match = BSON("x"
                         << "a b");
    BSONObj notMatch = BSON("x"
                            << "a\nb");
    RegexMatchExpression regex("", "a.b", "");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementDotAllOn) {
    BSONObj match = BSON("x"
                         << "a b");
    BSONObj matchDotAll = BSON("x"
                               << "a\nb");
    BSONObj notMatch = BSON("x"
                            << "ab");
    RegexMatchExpression regex("", "a.b", "s");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(regex.matchesSingleElement(matchDotAll.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementMultipleFlags) {
    BSONObj matchMultilineDotAll = BSON("x"
                                        << "\na\nb");
    RegexMatchExpression regex("", "^a.b", "ms");
    ASSERT(regex.matchesSingleElement(matchMultilineDotAll.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementRegexType) {
    BSONObj match = BSONObjBuilder().appendRegex("x", "yz", "i").obj();
    BSONObj notMatchPattern = BSONObjBuilder().appendRegex("x", "r", "i").obj();
    BSONObj notMatchFlags = BSONObjBuilder().appendRegex("x", "yz", "s").obj();
    RegexMatchExpression regex("", "yz", "i");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatchPattern.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatchFlags.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementSymbolType) {
    BSONObj match = BSONObjBuilder().appendSymbol("x", "yz").obj();
    BSONObj notMatch = BSONObjBuilder().appendSymbol("x", "gg").obj();
    RegexMatchExpression regex("", "yz", "");
    ASSERT(regex.matchesSingleElement(match.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementWrongType) {
    BSONObj notMatchInt = BSON("x" << 1);
    BSONObj notMatchBool = BSON("x" << true);
    RegexMatchExpression regex("", "1", "");
    ASSERT(!regex.matchesSingleElement(notMatchInt.firstElement()));
    ASSERT(!regex.matchesSingleElement(notMatchBool.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementUtf8) {
    BSONObj multiByteCharacter = BSON("x"
                                      << "\xc2\xa5");
    RegexMatchExpression regex("", "^.$", "");
    ASSERT(regex.matchesSingleElement(multiByteCharacter.firstElement()));
}

TEST(RegexMatchExpression, MatchesScalar) {
    RegexMatchExpression regex("a", "b", "");
    ASSERT(regex.matchesBSON(BSON("a"
                                  << "b"),
                             NULL));
    ASSERT(!regex.matchesBSON(BSON("a"
                                   << "c"),
                              NULL));
}

TEST(RegexMatchExpression, MatchesArrayValue) {
    RegexMatchExpression regex("a", "b", "");
    ASSERT(regex.matchesBSON(BSON("a" << BSON_ARRAY("c"
                                                    << "b")),
                             NULL));
    ASSERT(!regex.matchesBSON(BSON("a" << BSON_ARRAY("d"
                                                     << "c")),
                              NULL));
}

TEST(RegexMatchExpression, MatchesNull) {
    RegexMatchExpression regex("a", "b", "");
    ASSERT(!regex.matchesBSON(BSONObj(), NULL));
    ASSERT(!regex.matchesBSON(BSON("a" << BSONNULL), NULL));
}

TEST(RegexMatchExpression, ElemMatchKey) {
    RegexMatchExpression regex("a", "b", "");
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!regex.matchesBSON(BSON("a"
                                   << "c"),
                              &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(regex.matchesBSON(BSON("a"
                                  << "b"),
                             &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(regex.matchesBSON(BSON("a" << BSON_ARRAY("c"
                                                    << "b")),
                             &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(RegexMatchExpression, Equality1) {
    RegexMatchExpression r1("a", "b", "");
    RegexMatchExpression r2("a", "b", "x");
    RegexMatchExpression r3("a", "c", "");
    RegexMatchExpression r4("b", "b", "");

    ASSERT(r1.equivalent(&r1));
    ASSERT(!r1.equivalent(&r2));
    ASSERT(!r1.equivalent(&r3));
    ASSERT(!r1.equivalent(&r4));
}

TEST(RegexMatchExpression, RegexCannotContainEmbeddedNullByte) {
    {
        const auto embeddedNull = "a\0b"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression regex("path", embeddedNull, ""),
                           AssertionException,
                           ErrorCodes::BadValue);
    }

    {
        const auto singleNullByte = "\0"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression regex("path", singleNullByte, ""),
                           AssertionException,
                           ErrorCodes::BadValue);
    }

    {
        const auto leadingNullByte = "\0bbbb"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression regex("path", leadingNullByte, ""),
                           AssertionException,
                           ErrorCodes::BadValue);
    }

    {
        const auto trailingNullByte = "bbbb\0"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression regex("path", trailingNullByte, ""),
                           AssertionException,
                           ErrorCodes::BadValue);
    }
}

TEST(RegexMatchExpression, RegexOptionsStringCannotContainEmbeddedNullByte) {
    {
        const auto embeddedNull = "a\0b"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression regex("path", "pattern", embeddedNull),
                           AssertionException,
                           ErrorCodes::BadValue);
    }

    {
        const auto singleNullByte = "\0"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression regex("path", "pattern", singleNullByte),
                           AssertionException,
                           ErrorCodes::BadValue);
    }

    {
        const auto leadingNullByte = "\0bbbb"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression regex("path", "pattern", leadingNullByte),
                           AssertionException,
                           ErrorCodes::BadValue);
    }

    {
        const auto trailingNullByte = "bbbb\0"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression regex("path", "pattern", trailingNullByte),
                           AssertionException,
                           ErrorCodes::BadValue);
    }
}

TEST(RegexMatchExpression, MalformedRegexAcceptedButMatchesNothing) {
    RegexMatchExpression regex("a", "[(*ACCEPT)", "");
    ASSERT_FALSE(regex.matchesBSON(BSON("a"
                                        << "")));
    ASSERT_FALSE(regex.matchesBSON(BSON("a"
                                        << "[")));
}

TEST(RegexMatchExpression, RegexAcceptsUCPOption) {
    RegexMatchExpression regex("a", "(*UCP)(\\w|\u304C)", "");
    ASSERT(regex.matchesBSON(BSON("a"
                                  << "k")));
    ASSERT(regex.matchesBSON(BSON("a"
                                  << "\u304B")));
    ASSERT(regex.matchesBSON(BSON("a"
                                  << "\u304C")));
}

TEST(ModMatchExpression, MatchesElement) {
    BSONObj match = BSON("a" << 1);
    BSONObj largerMatch = BSON("a" << 4.0);
    BSONObj longLongMatch = BSON("a" << 68719476736LL);
    BSONObj notMatch = BSON("a" << 6);
    BSONObj negativeNotMatch = BSON("a" << -2);
    ModMatchExpression mod("", 3, 1);
    ASSERT(mod.matchesSingleElement(match.firstElement()));
    ASSERT(mod.matchesSingleElement(largerMatch.firstElement()));
    ASSERT(mod.matchesSingleElement(longLongMatch.firstElement()));
    ASSERT(!mod.matchesSingleElement(notMatch.firstElement()));
    ASSERT(!mod.matchesSingleElement(negativeNotMatch.firstElement()));
}

TEST(ModMatchExpression, ZeroDivisor) {
    ASSERT_THROWS_CODE(ModMatchExpression mod("", 0, 1), AssertionException, ErrorCodes::BadValue);
}

TEST(ModMatchExpression, MatchesScalar) {
    ModMatchExpression mod("a", 5, 2);
    ASSERT(mod.matchesBSON(BSON("a" << 7.0), NULL));
    ASSERT(!mod.matchesBSON(BSON("a" << 4), NULL));
}

TEST(ModMatchExpression, MatchesArrayValue) {
    ModMatchExpression mod("a", 5, 2);
    ASSERT(mod.matchesBSON(BSON("a" << BSON_ARRAY(5 << 12LL)), NULL));
    ASSERT(!mod.matchesBSON(BSON("a" << BSON_ARRAY(6 << 8)), NULL));
}

TEST(ModMatchExpression, MatchesNull) {
    ModMatchExpression mod("a", 5, 2);
    ASSERT(!mod.matchesBSON(BSONObj(), NULL));
    ASSERT(!mod.matchesBSON(BSON("a" << BSONNULL), NULL));
}

TEST(ModMatchExpression, ElemMatchKey) {
    ModMatchExpression mod("a", 5, 2);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!mod.matchesBSON(BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(mod.matchesBSON(BSON("a" << 2), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(mod.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(ModMatchExpression, Equality1) {
    ModMatchExpression m1("a", 1, 2);
    ModMatchExpression m2("a", 2, 2);
    ModMatchExpression m3("a", 1, 1);
    ModMatchExpression m4("b", 1, 2);

    ASSERT(m1.equivalent(&m1));
    ASSERT(!m1.equivalent(&m2));
    ASSERT(!m1.equivalent(&m3));
    ASSERT(!m1.equivalent(&m4));
}

TEST(ExistsMatchExpression, MatchesElement) {
    BSONObj existsInt = BSON("a" << 5);
    BSONObj existsNull = BSON("a" << BSONNULL);
    BSONObj doesntExist = BSONObj();
    ExistsMatchExpression exists("");
    ASSERT(exists.matchesSingleElement(existsInt.firstElement()));
    ASSERT(exists.matchesSingleElement(existsNull.firstElement()));
    ASSERT(!exists.matchesSingleElement(doesntExist.firstElement()));
}

TEST(ExistsMatchExpression, MatchesElementExistsTrueValue) {
    BSONObj exists = BSON("a" << 5);
    BSONObj missing = BSONObj();
    ExistsMatchExpression existsTrueValue("");
    ASSERT(existsTrueValue.matchesSingleElement(exists.firstElement()));
    ASSERT(!existsTrueValue.matchesSingleElement(missing.firstElement()));
}

TEST(ExistsMatchExpression, MatchesScalar) {
    ExistsMatchExpression exists("a");
    ASSERT(exists.matchesBSON(BSON("a" << 1), NULL));
    ASSERT(exists.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(!exists.matchesBSON(BSON("b" << 1), NULL));
}

TEST(ExistsMatchExpression, MatchesArray) {
    ExistsMatchExpression exists("a");
    ASSERT(exists.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5.5)), NULL));
}

TEST(ExistsMatchExpression, ElemMatchKey) {
    ExistsMatchExpression exists("a.b");
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exists.matchesBSON(BSON("a" << 1), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exists.matchesBSON(BSON("a" << BSON("b" << 6)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exists.matchesBSON(BSON("a" << BSON_ARRAY(2 << BSON("b" << 7))), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(ExistsMatchExpression, Equivalent) {
    ExistsMatchExpression e1("a");
    ExistsMatchExpression e2("b");

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
}

TEST(InMatchExpression, MatchesElementSingle) {
    BSONArray operand = BSON_ARRAY(1);
    BSONObj match = BSON("a" << 1);
    BSONObj notMatch = BSON("a" << 2);
    InMatchExpression in("");
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));
    ASSERT(in.matchesSingleElement(match["a"]));
    ASSERT(!in.matchesSingleElement(notMatch["a"]));
}

TEST(InMatchExpression, MatchesEmpty) {
    InMatchExpression in("a");

    BSONObj notMatch = BSON("a" << 2);
    ASSERT(!in.matchesSingleElement(notMatch["a"]));
    ASSERT(!in.matchesBSON(BSON("a" << 1), NULL));
    ASSERT(!in.matchesBSON(BSONObj(), NULL));
}

TEST(InMatchExpression, MatchesElementMultiple) {
    BSONObj operand = BSON_ARRAY(1 << "r" << true << 1);
    InMatchExpression in("");
    std::vector<BSONElement> equalities{operand[0], operand[1], operand[2], operand[3]};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    BSONObj matchFirst = BSON("a" << 1);
    BSONObj matchSecond = BSON("a"
                               << "r");
    BSONObj matchThird = BSON("a" << true);
    BSONObj notMatch = BSON("a" << false);
    ASSERT(in.matchesSingleElement(matchFirst["a"]));
    ASSERT(in.matchesSingleElement(matchSecond["a"]));
    ASSERT(in.matchesSingleElement(matchThird["a"]));
    ASSERT(!in.matchesSingleElement(notMatch["a"]));
}


TEST(InMatchExpression, MatchesScalar) {
    BSONObj operand = BSON_ARRAY(5);
    InMatchExpression in("a");
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT(in.matchesBSON(BSON("a" << 5.0), NULL));
    ASSERT(!in.matchesBSON(BSON("a" << 4), NULL));
}

TEST(InMatchExpression, MatchesArrayValue) {
    BSONObj operand = BSON_ARRAY(5);
    InMatchExpression in("a");
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT(in.matchesBSON(BSON("a" << BSON_ARRAY(5.0 << 6)), NULL));
    ASSERT(!in.matchesBSON(BSON("a" << BSON_ARRAY(6 << 7)), NULL));
    ASSERT(!in.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), NULL));
}

TEST(InMatchExpression, MatchesNull) {
    BSONObj operand = BSON_ARRAY(BSONNULL);

    InMatchExpression in("a");
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT(in.matchesBSON(BSONObj(), NULL));
    ASSERT(in.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(!in.matchesBSON(BSON("a" << 4), NULL));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(in.matchesBSON(BSON("b" << 4), NULL));
}

TEST(InMatchExpression, MatchesUndefined) {
    BSONObj operand = BSON_ARRAY(BSONUndefined);

    InMatchExpression in("a");
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_NOT_OK(in.setEqualities(std::move(equalities)));
}

TEST(InMatchExpression, MatchesMinKey) {
    BSONObj operand = BSON_ARRAY(MinKey);
    InMatchExpression in("a");
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT(in.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(!in.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(!in.matchesBSON(BSON("a" << 4), NULL));
}

TEST(InMatchExpression, MatchesMaxKey) {
    BSONObj operand = BSON_ARRAY(MaxKey);
    InMatchExpression in("a");
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT(in.matchesBSON(BSON("a" << MaxKey), NULL));
    ASSERT(!in.matchesBSON(BSON("a" << MinKey), NULL));
    ASSERT(!in.matchesBSON(BSON("a" << 4), NULL));
}

TEST(InMatchExpression, MatchesFullArray) {
    BSONObj operand = BSON_ARRAY(BSON_ARRAY(1 << 2) << 4 << 5);
    InMatchExpression in("a");
    std::vector<BSONElement> equalities{operand[0], operand[1], operand[2]};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT(in.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2)), NULL));
    ASSERT(!in.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 3)), NULL));
    ASSERT(!in.matchesBSON(BSON("a" << BSON_ARRAY(1)), NULL));
    ASSERT(!in.matchesBSON(BSON("a" << 1), NULL));
}

TEST(InMatchExpression, ElemMatchKey) {
    BSONObj operand = BSON_ARRAY(5 << 2);
    InMatchExpression in("a");
    std::vector<BSONElement> equalities{operand[0], operand[1]};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!in.matchesBSON(BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(in.matchesBSON(BSON("a" << 5), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(in.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(InMatchExpression, InMatchExpressionsWithDifferentNumbersOfElementsAreUnequal) {
    BSONObj obj = BSON(""
                       << "string");
    InMatchExpression eq1("");
    InMatchExpression eq2("");
    std::vector<BSONElement> equalities{obj.firstElement()};
    ASSERT_OK(eq1.setEqualities(std::move(equalities)));
    ASSERT(!eq1.equivalent(&eq2));
}

TEST(InMatchExpression, InMatchExpressionsWithUnequalCollatorsAreUnequal) {
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kReverseString);
    InMatchExpression eq1("");
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression eq2("");
    eq2.setCollator(&collator2);
    ASSERT(!eq1.equivalent(&eq2));
}

TEST(InMatchExpression, InMatchExpressionsWithEqualCollatorsAreEqual) {
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression eq1("");
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression eq2("");
    eq2.setCollator(&collator2);
    ASSERT(eq1.equivalent(&eq2));
}

TEST(InMatchExpression, InMatchExpressionsWithCollationEquivalentElementsAreEqual) {
    BSONObj obj1 = BSON(""
                        << "string1");
    BSONObj obj2 = BSON(""
                        << "string2");
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression eq1("");
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression eq2("");
    eq2.setCollator(&collator2);

    std::vector<BSONElement> equalities1{obj1.firstElement()};
    ASSERT_OK(eq1.setEqualities(std::move(equalities1)));

    std::vector<BSONElement> equalities2{obj2.firstElement()};
    ASSERT_OK(eq2.setEqualities(std::move(equalities2)));

    ASSERT(eq1.equivalent(&eq2));
}

TEST(InMatchExpression, InMatchExpressionsWithCollationNonEquivalentElementsAreUnequal) {
    BSONObj obj1 = BSON(""
                        << "string1");
    BSONObj obj2 = BSON(""
                        << "string2");
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kReverseString);
    InMatchExpression eq1("");
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kReverseString);
    InMatchExpression eq2("");
    eq2.setCollator(&collator2);

    std::vector<BSONElement> equalities1{obj1.firstElement()};
    ASSERT_OK(eq1.setEqualities(std::move(equalities1)));

    std::vector<BSONElement> equalities2{obj2.firstElement()};
    ASSERT_OK(eq2.setEqualities(std::move(equalities2)));

    ASSERT(!eq1.equivalent(&eq2));
}

TEST(InMatchExpression, StringMatchingWithNullCollatorUsesBinaryComparison) {
    BSONArray operand = BSON_ARRAY("string");
    BSONObj notMatch = BSON("a"
                            << "string2");
    InMatchExpression in("");
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));
    ASSERT(!in.matchesSingleElement(notMatch["a"]));
}

TEST(InMatchExpression, StringMatchingRespectsCollation) {
    BSONArray operand = BSON_ARRAY("string");
    BSONObj match = BSON("a"
                         << "string2");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression in("");
    in.setCollator(&collator);
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));
    ASSERT(in.matchesSingleElement(match["a"]));
}

TEST(InMatchExpression, ChangingCollationAfterAddingEqualitiesPreservesEqualities) {
    BSONObj obj1 = BSON(""
                        << "string1");
    BSONObj obj2 = BSON(""
                        << "string2");
    CollatorInterfaceMock collatorAlwaysEqual(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock collatorReverseString(CollatorInterfaceMock::MockType::kReverseString);
    InMatchExpression in("");
    in.setCollator(&collatorAlwaysEqual);
    std::vector<BSONElement> equalities{obj1.firstElement(), obj2.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));
    ASSERT(in.getEqualities().size() == 1);
    in.setCollator(&collatorReverseString);
    ASSERT(in.getEqualities().size() == 2);
    ASSERT(in.getEqualities().count(obj1.firstElement()));
    ASSERT(in.getEqualities().count(obj2.firstElement()));
}

std::vector<uint32_t> bsonArrayToBitPositions(const BSONArray& ba) {
    std::vector<uint32_t> bitPositions;

    // Convert BSONArray of bit positions to int vector
    for (const auto& elt : ba) {
        bitPositions.push_back(elt._numberInt());
    }

    return bitPositions;
}

TEST(BitTestMatchExpression, DoesNotMatchOther) {
    std::vector<uint32_t> bitPositions;

    BSONObj notMatch1 = fromjson("{a: {}}");     // Object
    BSONObj notMatch2 = fromjson("{a: null}");   // Null
    BSONObj notMatch3 = fromjson("{a: []}");     // Array
    BSONObj notMatch4 = fromjson("{a: true}");   // Boolean
    BSONObj notMatch5 = fromjson("{a: ''}");     // String
    BSONObj notMatch6 = fromjson("{a: 5.5}");    // Non-integral Double
    BSONObj notMatch7 = fromjson("{a: NaN}");    // NaN
    BSONObj notMatch8 = fromjson("{a: 1e100}");  // Too-Large Double
    BSONObj notMatch9 = fromjson("{a: ObjectId('000000000000000000000000')}");  // OID
    BSONObj notMatch10 = fromjson("{a: Date(54)}");                             // Date

    BitsAllSetMatchExpression balls("a", bitPositions);
    BitsAllClearMatchExpression ballc("a", bitPositions);
    BitsAnySetMatchExpression banys("a", bitPositions);
    BitsAnyClearMatchExpression banyc("a", bitPositions);

    ASSERT_EQ((size_t)0, balls.numBitPositions());
    ASSERT_EQ((size_t)0, ballc.numBitPositions());
    ASSERT_EQ((size_t)0, banys.numBitPositions());
    ASSERT_EQ((size_t)0, banyc.numBitPositions());
    ASSERT(!balls.matchesSingleElement(notMatch1["a"]));
    ASSERT(!balls.matchesSingleElement(notMatch2["a"]));
    ASSERT(!balls.matchesSingleElement(notMatch3["a"]));
    ASSERT(!balls.matchesSingleElement(notMatch4["a"]));
    ASSERT(!balls.matchesSingleElement(notMatch5["a"]));
    ASSERT(!balls.matchesSingleElement(notMatch6["a"]));
    ASSERT(!balls.matchesSingleElement(notMatch7["a"]));
    ASSERT(!balls.matchesSingleElement(notMatch8["a"]));
    ASSERT(!balls.matchesSingleElement(notMatch9["a"]));
    ASSERT(!balls.matchesSingleElement(notMatch10["a"]));
    ASSERT(!ballc.matchesSingleElement(notMatch1["a"]));
    ASSERT(!ballc.matchesSingleElement(notMatch2["a"]));
    ASSERT(!ballc.matchesSingleElement(notMatch3["a"]));
    ASSERT(!ballc.matchesSingleElement(notMatch4["a"]));
    ASSERT(!ballc.matchesSingleElement(notMatch5["a"]));
    ASSERT(!ballc.matchesSingleElement(notMatch6["a"]));
    ASSERT(!ballc.matchesSingleElement(notMatch7["a"]));
    ASSERT(!ballc.matchesSingleElement(notMatch8["a"]));
    ASSERT(!ballc.matchesSingleElement(notMatch9["a"]));
    ASSERT(!ballc.matchesSingleElement(notMatch10["a"]));
    ASSERT(!banys.matchesSingleElement(notMatch1["a"]));
    ASSERT(!banys.matchesSingleElement(notMatch2["a"]));
    ASSERT(!banys.matchesSingleElement(notMatch3["a"]));
    ASSERT(!banys.matchesSingleElement(notMatch4["a"]));
    ASSERT(!banys.matchesSingleElement(notMatch5["a"]));
    ASSERT(!banys.matchesSingleElement(notMatch6["a"]));
    ASSERT(!banys.matchesSingleElement(notMatch7["a"]));
    ASSERT(!banys.matchesSingleElement(notMatch8["a"]));
    ASSERT(!banys.matchesSingleElement(notMatch9["a"]));
    ASSERT(!banys.matchesSingleElement(notMatch10["a"]));
    ASSERT(!banyc.matchesSingleElement(notMatch1["a"]));
    ASSERT(!banyc.matchesSingleElement(notMatch2["a"]));
    ASSERT(!banyc.matchesSingleElement(notMatch3["a"]));
    ASSERT(!banyc.matchesSingleElement(notMatch4["a"]));
    ASSERT(!banyc.matchesSingleElement(notMatch5["a"]));
    ASSERT(!banyc.matchesSingleElement(notMatch6["a"]));
    ASSERT(!banyc.matchesSingleElement(notMatch7["a"]));
    ASSERT(!banyc.matchesSingleElement(notMatch8["a"]));
    ASSERT(!banyc.matchesSingleElement(notMatch9["a"]));
    ASSERT(!banyc.matchesSingleElement(notMatch10["a"]));
}

TEST(BitTestMatchExpression, MatchBinaryWithLongBitMask) {
    long long bitMask = 54;

    BSONObj match = fromjson("{a: {$binary: 'NgAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");

    BitsAllSetMatchExpression balls("a", bitMask);
    BitsAllClearMatchExpression ballc("a", bitMask);
    BitsAnySetMatchExpression banys("a", bitMask);
    BitsAnyClearMatchExpression banyc("a", bitMask);

    std::vector<uint32_t> bitPositions = balls.getBitPositions();
    ASSERT(balls.matchesSingleElement(match["a"]));
    ASSERT(!ballc.matchesSingleElement(match["a"]));
    ASSERT(banys.matchesSingleElement(match["a"]));
    ASSERT(!banyc.matchesSingleElement(match["a"]));
}

TEST(BitTestMatchExpression, MatchLongWithBinaryBitMask) {
    const char* bitMaskSet = "\x36\x00\x00\x00";
    const char* bitMaskClear = "\xC9\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF";

    BSONObj match = fromjson("{a: 54}");

    BitsAllSetMatchExpression balls("a", bitMaskSet, 4);
    BitsAllClearMatchExpression ballc("a", bitMaskClear, 9);
    BitsAnySetMatchExpression banys("a", bitMaskSet, 4);
    BitsAnyClearMatchExpression banyc("a", bitMaskClear, 9);

    ASSERT(balls.matchesSingleElement(match["a"]));
    ASSERT(ballc.matchesSingleElement(match["a"]));
    ASSERT(banys.matchesSingleElement(match["a"]));
    ASSERT(banyc.matchesSingleElement(match["a"]));
}

TEST(BitTestMatchExpression, MatchesEmpty) {
    std::vector<uint32_t> bitPositions;

    BSONObj match1 = fromjson("{a: NumberInt(54)}");
    BSONObj match2 = fromjson("{a: NumberLong(54)}");
    BSONObj match3 = fromjson("{a: 54.0}");
    BSONObj match4 = fromjson("{a: {$binary: '2AAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");

    BitsAllSetMatchExpression balls("a", bitPositions);
    BitsAllClearMatchExpression ballc("a", bitPositions);
    BitsAnySetMatchExpression banys("a", bitPositions);
    BitsAnyClearMatchExpression banyc("a", bitPositions);

    ASSERT_EQ((size_t)0, balls.numBitPositions());
    ASSERT_EQ((size_t)0, ballc.numBitPositions());
    ASSERT_EQ((size_t)0, banys.numBitPositions());
    ASSERT_EQ((size_t)0, banyc.numBitPositions());
    ASSERT(balls.matchesSingleElement(match1["a"]));
    ASSERT(balls.matchesSingleElement(match2["a"]));
    ASSERT(balls.matchesSingleElement(match3["a"]));
    ASSERT(balls.matchesSingleElement(match4["a"]));
    ASSERT(ballc.matchesSingleElement(match1["a"]));
    ASSERT(ballc.matchesSingleElement(match2["a"]));
    ASSERT(ballc.matchesSingleElement(match3["a"]));
    ASSERT(ballc.matchesSingleElement(match4["a"]));
    ASSERT(!banys.matchesSingleElement(match1["a"]));
    ASSERT(!banys.matchesSingleElement(match2["a"]));
    ASSERT(!banys.matchesSingleElement(match3["a"]));
    ASSERT(!banys.matchesSingleElement(match4["a"]));
    ASSERT(!banyc.matchesSingleElement(match1["a"]));
    ASSERT(!banyc.matchesSingleElement(match2["a"]));
    ASSERT(!banyc.matchesSingleElement(match3["a"]));
    ASSERT(!banyc.matchesSingleElement(match4["a"]));
}

TEST(BitTestMatchExpression, MatchesInteger) {
    BSONArray bas = BSON_ARRAY(1 << 2 << 4 << 5);
    BSONArray bac = BSON_ARRAY(0 << 3 << 600);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: NumberInt(54)}");
    BSONObj match2 = fromjson("{a: NumberLong(54)}");
    BSONObj match3 = fromjson("{a: 54.0}");

    BitsAllSetMatchExpression balls("a", bitPositionsSet);
    BitsAllClearMatchExpression ballc("a", bitPositionsClear);
    BitsAnySetMatchExpression banys("a", bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a", bitPositionsClear);

    ASSERT_EQ((size_t)4, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)4, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(balls.matchesSingleElement(match1["a"]));
    ASSERT(balls.matchesSingleElement(match2["a"]));
    ASSERT(balls.matchesSingleElement(match3["a"]));
    ASSERT(ballc.matchesSingleElement(match1["a"]));
    ASSERT(ballc.matchesSingleElement(match2["a"]));
    ASSERT(ballc.matchesSingleElement(match3["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match3["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match3["a"]));
}

TEST(BitTestMatchExpression, MatchesNegativeInteger) {
    BSONArray bas = BSON_ARRAY(1 << 3 << 6 << 7 << 33);
    BSONArray bac = BSON_ARRAY(0 << 2 << 4 << 5);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: NumberInt(-54)}");
    BSONObj match2 = fromjson("{a: NumberLong(-54)}");
    BSONObj match3 = fromjson("{a: -54.0}");

    BitsAllSetMatchExpression balls("a", bitPositionsSet);
    BitsAllClearMatchExpression ballc("a", bitPositionsClear);
    BitsAnySetMatchExpression banys("a", bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a", bitPositionsClear);

    ASSERT_EQ((size_t)5, balls.numBitPositions());
    ASSERT_EQ((size_t)4, ballc.numBitPositions());
    ASSERT_EQ((size_t)5, banys.numBitPositions());
    ASSERT_EQ((size_t)4, banyc.numBitPositions());
    ASSERT(balls.matchesSingleElement(match1["a"]));
    ASSERT(balls.matchesSingleElement(match2["a"]));
    ASSERT(balls.matchesSingleElement(match3["a"]));
    ASSERT(ballc.matchesSingleElement(match1["a"]));
    ASSERT(ballc.matchesSingleElement(match2["a"]));
    ASSERT(ballc.matchesSingleElement(match3["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match3["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match3["a"]));
}

TEST(BitTestMatchExpression, MatchesIntegerWithBitMask) {
    long long bitMaskSet = 54;
    long long bitMaskClear = 201;

    BSONObj match1 = fromjson("{a: NumberInt(54)}");
    BSONObj match2 = fromjson("{a: NumberLong(54)}");
    BSONObj match3 = fromjson("{a: 54.0}");

    BitsAllSetMatchExpression balls("a", bitMaskSet);
    BitsAllClearMatchExpression ballc("a", bitMaskClear);
    BitsAnySetMatchExpression banys("a", bitMaskSet);
    BitsAnyClearMatchExpression banyc("a", bitMaskClear);

    ASSERT(balls.matchesSingleElement(match1["a"]));
    ASSERT(balls.matchesSingleElement(match2["a"]));
    ASSERT(balls.matchesSingleElement(match3["a"]));
    ASSERT(ballc.matchesSingleElement(match1["a"]));
    ASSERT(ballc.matchesSingleElement(match2["a"]));
    ASSERT(ballc.matchesSingleElement(match3["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match3["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match3["a"]));
}

TEST(BitTestMatchExpression, MatchesNegativeIntegerWithBitMask) {
    long long bitMaskSet = 10;
    long long bitMaskClear = 5;

    BSONObj match1 = fromjson("{a: NumberInt(-54)}");
    BSONObj match2 = fromjson("{a: NumberLong(-54)}");
    BSONObj match3 = fromjson("{a: -54.0}");

    BitsAllSetMatchExpression balls("a", bitMaskSet);
    BitsAllClearMatchExpression ballc("a", bitMaskClear);
    BitsAnySetMatchExpression banys("a", bitMaskSet);
    BitsAnyClearMatchExpression banyc("a", bitMaskClear);

    ASSERT(balls.matchesSingleElement(match1["a"]));
    ASSERT(balls.matchesSingleElement(match2["a"]));
    ASSERT(balls.matchesSingleElement(match3["a"]));
    ASSERT(ballc.matchesSingleElement(match1["a"]));
    ASSERT(ballc.matchesSingleElement(match2["a"]));
    ASSERT(ballc.matchesSingleElement(match3["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match3["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match3["a"]));
}

TEST(BitTestMatchExpression, DoesNotMatchInteger) {
    BSONArray bas = BSON_ARRAY(1 << 2 << 4 << 5 << 6);
    BSONArray bac = BSON_ARRAY(0 << 3 << 1);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: NumberInt(54)}");
    BSONObj match2 = fromjson("{a: NumberLong(54)}");
    BSONObj match3 = fromjson("{a: 54.0}");

    BitsAllSetMatchExpression balls("a", bitPositionsSet);
    BitsAllClearMatchExpression ballc("a", bitPositionsClear);
    BitsAnySetMatchExpression banys("a", bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a", bitPositionsClear);

    ASSERT_EQ((size_t)5, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)5, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(!balls.matchesSingleElement(match1["a"]));
    ASSERT(!balls.matchesSingleElement(match2["a"]));
    ASSERT(!balls.matchesSingleElement(match3["a"]));
    ASSERT(!ballc.matchesSingleElement(match1["a"]));
    ASSERT(!ballc.matchesSingleElement(match2["a"]));
    ASSERT(!ballc.matchesSingleElement(match3["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match3["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match3["a"]));
}

TEST(BitTestMatchExpression, DoesNotMatchIntegerWithBitMask) {
    long long bitMaskSet = 118;
    long long bitMaskClear = 11;

    BSONObj match1 = fromjson("{a: NumberInt(54)}");
    BSONObj match2 = fromjson("{a: NumberLong(54)}");
    BSONObj match3 = fromjson("{a: 54.0}");

    BitsAllSetMatchExpression balls("a", bitMaskSet);
    BitsAllClearMatchExpression ballc("a", bitMaskClear);
    BitsAnySetMatchExpression banys("a", bitMaskSet);
    BitsAnyClearMatchExpression banyc("a", bitMaskClear);

    ASSERT(!balls.matchesSingleElement(match1["a"]));
    ASSERT(!balls.matchesSingleElement(match2["a"]));
    ASSERT(!balls.matchesSingleElement(match3["a"]));
    ASSERT(!ballc.matchesSingleElement(match1["a"]));
    ASSERT(!ballc.matchesSingleElement(match2["a"]));
    ASSERT(!ballc.matchesSingleElement(match3["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match3["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match3["a"]));
}

TEST(BitTestMatchExpression, MatchesBinary1) {
    BSONArray bas = BSON_ARRAY(1 << 2 << 4 << 5);
    BSONArray bac = BSON_ARRAY(0 << 3 << 600);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: {$binary: 'NgAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00110110...
    BSONObj match2 = fromjson("{a: {$binary: 'NgAjqwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: 00110110...

    BitsAllSetMatchExpression balls("a", bitPositionsSet);
    BitsAllClearMatchExpression ballc("a", bitPositionsClear);
    BitsAnySetMatchExpression banys("a", bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a", bitPositionsClear);

    ASSERT_EQ((size_t)4, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)4, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(balls.matchesSingleElement(match1["a"]));
    ASSERT(balls.matchesSingleElement(match2["a"]));
    ASSERT(ballc.matchesSingleElement(match1["a"]));
    ASSERT(ballc.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
}

TEST(BitTestMatchExpression, MatchesBinary2) {
    BSONArray bas = BSON_ARRAY(21 << 22 << 8 << 9);
    BSONArray bac = BSON_ARRAY(20 << 23 << 612);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: {$binary: 'AANgAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00000000 00000011 01100000
    BSONObj match2 = fromjson("{a: {$binary: 'JANgqwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: ........ 00000011 01100000

    BitsAllSetMatchExpression balls("a", bitPositionsSet);
    BitsAllClearMatchExpression ballc("a", bitPositionsClear);
    BitsAnySetMatchExpression banys("a", bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a", bitPositionsClear);

    ASSERT_EQ((size_t)4, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)4, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(balls.matchesSingleElement(match1["a"]));
    ASSERT(balls.matchesSingleElement(match2["a"]));
    ASSERT(ballc.matchesSingleElement(match1["a"]));
    ASSERT(ballc.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
}

TEST(BitTestMatchExpression, MatchesBinaryWithBitMask) {
    const char* bas = "\0\x03\x60\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
    const char* bac = "\0\xFC\x9F\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

    BSONObj match1 = fromjson("{a: {$binary: 'AANgAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00000000 00000011 01100000
    BSONObj match2 = fromjson("{a: {$binary: 'JANgAwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: ........ 00000011 01100000

    BitsAllSetMatchExpression balls("a", bas, 21);
    BitsAllClearMatchExpression ballc("a", bac, 21);
    BitsAnySetMatchExpression banys("a", bas, 21);
    BitsAnyClearMatchExpression banyc("a", bac, 21);

    ASSERT(balls.matchesSingleElement(match1["a"]));
    ASSERT(balls.matchesSingleElement(match2["a"]));
    ASSERT(ballc.matchesSingleElement(match1["a"]));
    ASSERT(ballc.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
}

TEST(BitTestMatchExpression, DoesNotMatchBinary1) {
    BSONArray bas = BSON_ARRAY(1 << 2 << 4 << 5 << 6);
    BSONArray bac = BSON_ARRAY(0 << 3 << 1);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: {$binary: 'NgAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00110110...
    BSONObj match2 = fromjson("{a: {$binary: 'NgAjqwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: 00110110...

    BitsAllSetMatchExpression balls("a", bitPositionsSet);
    BitsAllClearMatchExpression ballc("a", bitPositionsClear);
    BitsAnySetMatchExpression banys("a", bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a", bitPositionsClear);

    ASSERT_EQ((size_t)5, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)5, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(!balls.matchesSingleElement(match1["a"]));
    ASSERT(!balls.matchesSingleElement(match2["a"]));
    ASSERT(!ballc.matchesSingleElement(match1["a"]));
    ASSERT(!ballc.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
}

TEST(BitTestMatchExpression, DoesNotMatchBinary2) {
    BSONArray bas = BSON_ARRAY(21 << 22 << 23 << 24 << 25);
    BSONArray bac = BSON_ARRAY(20 << 23 << 21);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: {$binary: 'AANgAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00000000 00000011 01100000
    BSONObj match2 = fromjson("{a: {$binary: 'JANgqwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: ........ 00000011 01100000

    BitsAllSetMatchExpression balls("a", bitPositionsSet);
    BitsAllClearMatchExpression ballc("a", bitPositionsClear);
    BitsAnySetMatchExpression banys("a", bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a", bitPositionsClear);

    ASSERT_EQ((size_t)5, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)5, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(!balls.matchesSingleElement(match1["a"]));
    ASSERT(!balls.matchesSingleElement(match2["a"]));
    ASSERT(!ballc.matchesSingleElement(match1["a"]));
    ASSERT(!ballc.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
}

TEST(BitTestMatchExpression, DoesNotMatchBinaryWithBitMask) {
    const char* bas = "\0\x03\x60\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\xFF";
    const char* bac = "\0\xFD\x9F\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\xFF";

    BSONObj match1 = fromjson("{a: {$binary: 'AANgAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00000000 00000011 01100000
    BSONObj match2 = fromjson("{a: {$binary: 'JANgAwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: ........ 00000011 01100000

    BitsAllSetMatchExpression balls("a", bas, 22);
    BitsAllClearMatchExpression ballc("a", bac, 22);
    BitsAnySetMatchExpression banys("a", bas, 22);
    BitsAnyClearMatchExpression banyc("a", bac, 22);
    ASSERT(!balls.matchesSingleElement(match1["a"]));
    ASSERT(!balls.matchesSingleElement(match2["a"]));
    ASSERT(!ballc.matchesSingleElement(match1["a"]));
    ASSERT(!ballc.matchesSingleElement(match2["a"]));
    ASSERT(banys.matchesSingleElement(match1["a"]));
    ASSERT(banys.matchesSingleElement(match2["a"]));
    ASSERT(banyc.matchesSingleElement(match1["a"]));
    ASSERT(banyc.matchesSingleElement(match2["a"]));
}
}
