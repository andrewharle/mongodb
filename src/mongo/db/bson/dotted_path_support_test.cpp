
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

#include <set>
#include <vector>

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

namespace dps = ::mongo::dotted_path_support;

TEST(DottedPathSupport, CompareObjectsAccordingToSort) {
    ASSERT_LT(dps::compareObjectsAccordingToSort(
                  BSON("a" << 1), BSON("a" << 2), BSON("b" << 1 << "a" << 1)),
              0);
    ASSERT_EQ(
        dps::compareObjectsAccordingToSort(BSON("a" << BSONNULL), BSON("b" << 1), BSON("a" << 1)),
        0);
}

TEST(DottedPathSupport, ExtractElementAtPath) {
    BSONObj obj = BSON("a" << 1 << "b" << BSON("a" << 2) << "c"
                           << BSON_ARRAY(BSON("a" << 3) << BSON("a" << 4)));
    ASSERT_EQUALS(1, dps::extractElementAtPath(obj, "a").numberInt());
    ASSERT_EQUALS(2, dps::extractElementAtPath(obj, "b.a").numberInt());
    ASSERT_EQUALS(3, dps::extractElementAtPath(obj, "c.0.a").numberInt());
    ASSERT_EQUALS(4, dps::extractElementAtPath(obj, "c.1.a").numberInt());

    ASSERT_TRUE(dps::extractElementAtPath(obj, "x").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "a.x").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "x.y").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, ".").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "..").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "...").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "a.").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, ".a").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "b.a.").eoo());
}

TEST(DottedPathSupport, ExtractElementsBasedOnTemplate) {
    BSONObj obj = BSON("a" << 10 << "b" << 11);

    ASSERT_EQ(BSON("a" << 10).woCompare(dps::extractElementsBasedOnTemplate(obj, BSON("a" << 1))),
              0);
    ASSERT_EQ(BSON("b" << 11).woCompare(dps::extractElementsBasedOnTemplate(obj, BSON("b" << 1))),
              0);
    ASSERT_EQ(obj.woCompare(dps::extractElementsBasedOnTemplate(obj, BSON("a" << 1 << "b" << 1))),
              0);

    ASSERT_EQ(dps::extractElementsBasedOnTemplate(obj, BSON("a" << 1 << "c" << 1))
                  .firstElement()
                  .fieldNameStringData(),
              "a");
}

void dumpBSONElementSet(const BSONElementSet& elements, StringBuilder* sb) {
    *sb << "[ ";
    bool firstIteration = true;
    for (auto&& elem : elements) {
        if (!firstIteration) {
            *sb << ", ";
        }
        *sb << "'" << elem << "'";
        firstIteration = false;
    }
    *sb << " ]";
}

void assertBSONElementSetsAreEqual(const std::vector<BSONObj>& expectedObjs,
                                   const BSONElementSet& actualElements) {
    BSONElementSet expectedElements;
    for (auto&& obj : expectedObjs) {
        expectedElements.insert(obj.firstElement());
    }

    if (expectedElements.size() != actualElements.size()) {
        StringBuilder sb;
        sb << "Expected set to contain " << expectedElements.size()
           << " element(s), but actual set contains " << actualElements.size()
           << " element(s); Expected set: ";
        dumpBSONElementSet(expectedElements, &sb);
        sb << ", Actual set: ";
        dumpBSONElementSet(actualElements, &sb);
        FAIL(sb.str());
    }

    // We do our own comparison of the two BSONElementSets because BSONElement::operator== considers
    // the field name.
    auto expectedIt = expectedElements.begin();
    auto actualIt = actualElements.begin();


    BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore,
                                 &SimpleStringDataComparator::kInstance);
    for (size_t i = 0; i < expectedElements.size(); ++i) {
        if (eltCmp.evaluate(*expectedIt != *actualIt)) {
            StringBuilder sb;
            sb << "Element '" << *expectedIt << "' doesn't have the same value as element '"
               << *actualIt << "'; Expected set: ";
            dumpBSONElementSet(expectedElements, &sb);
            sb << ", Actual set: ";
            dumpBSONElementSet(actualElements, &sb);
            FAIL(sb.str());
        }

        ++expectedIt;
        ++actualIt;
    }
}

void dumpArrayComponents(const std::set<size_t>& arrayComponents, StringBuilder* sb) {
    *sb << "[ ";
    bool firstIteration = true;
    for (const auto pos : arrayComponents) {
        if (!firstIteration) {
            *sb << ", ";
        }
        *sb << pos;
        firstIteration = false;
    }
    *sb << " ]";
}

void assertArrayComponentsAreEqual(const std::set<size_t>& expectedArrayComponents,
                                   const std::set<size_t>& actualArrayComponents) {
    if (expectedArrayComponents != actualArrayComponents) {
        StringBuilder sb;
        sb << "Expected: ";
        dumpArrayComponents(expectedArrayComponents, &sb);
        sb << ", Actual: ";
        dumpArrayComponents(actualArrayComponents, &sb);
        FAIL(sb.str());
    }
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithScalarValue) {
    BSONObj obj = BSON("a" << BSON("b" << 1));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1)}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithEmptyArrayValue) {
    BSONObj obj = BSON("a" << BSON("b" << BSONArrayBuilder().arr()));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual(std::vector<BSONObj>{}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithEmptyArrayValueAndExpandParamIsFalse) {
    BSONObj obj(fromjson("{a: {b: []}}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = false;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << BSONArray())}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithSingletonArrayValue) {
    BSONObj obj = BSON("a" << BSON("b" << BSON_ARRAY(1)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1)}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithSingletonArrayValueAndExpandParamIsFalse) {
    BSONObj obj(fromjson("{a: {b: {c: [3]}}}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = false;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b.c", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << BSON_ARRAY(3))}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithArrayValue) {
    BSONObj obj = BSON("a" << BSON("b" << BSON_ARRAY(1 << 2 << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1), BSON("" << 2), BSON("" << 3)}, actualElements);
    assertArrayComponentsAreEqual({1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, ObjectWithArrayOfSubobjectsWithScalarValue) {
    BSONObj obj = BSON("a" << BSON_ARRAY(BSON("b" << 1) << BSON("b" << 2) << BSON("b" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1), BSON("" << 2), BSON("" << 3)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, ObjectWithArrayOfSubobjectsWithArrayValues) {
    BSONObj obj =
        BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(1 << 2)) << BSON("b" << BSON_ARRAY(2 << 3))
                                                               << BSON("b" << BSON_ARRAY(3 << 1))));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1), BSON("" << 2), BSON("" << 3)}, actualElements);
    assertArrayComponentsAreEqual({0U, 1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath,
     ObjectWithArrayOfSubobjectsWithArrayValuesButNotExpandingTrailingArrayValues) {
    BSONObj obj = BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(1)) << BSON("b" << BSON_ARRAY(2))
                                                                    << BSON("b" << BSON_ARRAY(3))));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = false;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual(
        {BSON("" << BSON_ARRAY(1)), BSON("" << BSON_ARRAY(2)), BSON("" << BSON_ARRAY(3))},
        actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesNotExpandArrayWithinTrailingArray) {
    BSONObj obj = BSON("a" << BSON("b" << BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(3 << 4))));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << BSON_ARRAY(1 << 2)), BSON("" << BSON_ARRAY(3 << 4))},
                                  actualElements);
    assertArrayComponentsAreEqual({1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, ObjectWithTwoDimensionalArrayOfSubobjects) {
    // Does not expand the array within the array.
    BSONObj obj = fromjson("{a: [[{b: 0}, {b: 1}], [{b: 2}, {b: 3}]]}");

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, ObjectWithDiverseStructure) {
    BSONObj obj = fromjson(
        "{a: ["
        "     {b: 0},"
        "     [{b: 1}, {b: {c: -1}}],"
        "     'no b here!',"
        "     {b: [{c: -2}, 'no c here!']},"
        "     {b: {c: [-3, -4]}}"
        "]}");

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b.c", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << -2), BSON("" << -3), BSON("" << -4)}, actualElements);
    assertArrayComponentsAreEqual({0U, 1U, 2U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, AcceptsNumericFieldNames) {
    BSONObj obj = BSON("a" << BSON("0" << 1));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.0", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1)}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, UsesNumericFieldNameToExtractElementFromArray) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("0" << 2)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.0", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1)}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, TreatsNegativeIndexAsFieldName) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("-1" << 2) << BSON("b" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.-1", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, ExtractsNoValuesFromOutOfBoundsIndex) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("b" << 2) << BSON("10" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.10", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesNotTreatHexStringAsIndexSpecification) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("0x2" << 2) << BSON("NOT THIS ONE" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.0x2", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesNotAcceptLeadingPlusAsArrayIndex) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("+2" << 2) << BSON("NOT THIS ONE" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.+2", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesNotAcceptTrailingCharactersForArrayIndex) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("2xyz" << 2) << BSON("NOT THIS ONE" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.2xyz", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesNotAcceptNonDigitsForArrayIndex) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("2x4" << 2) << BSON("NOT THIS ONE" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.2x4", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath,
     DoesExtractNestedValuesFromWithinArraysTraversedWithPositionalPaths) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("2" << 2) << BSON("target" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.2.target", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 3)}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesExpandMultiplePositionalPathSpecifications) {
    BSONObj obj(fromjson("{a: [[{b: '(0, 0)'}, {b: '(0, 1)'}], [{b: '(1, 0)'}, {b: '(1, 1)'}]]}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.1.0.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON(""
                                        << "(1, 0)")},
                                  actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesAcceptNumericInitialField) {
    BSONObj obj = BSON("a" << 1 << "0" << 2);

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "0", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesExpandArrayFoundAfterPositionalSpecification) {
    BSONObj obj(fromjson("{a: [[{b: '(0, 0)'}, {b: '(0, 1)'}], [{b: '(1, 0)'}, {b: '(1, 1)'}]]}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.1.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON(""
                                        << "(1, 0)"),
                                   BSON(""
                                        << "(1, 1)")},
                                  actualElements);
    assertArrayComponentsAreEqual({1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, PositionalElementsNotConsideredArrayComponents) {
    BSONObj obj(fromjson("{a: [{b: [1, 2]}]}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.0.b.1", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, TrailingArrayIsExpandedEvenIfPositional) {
    BSONObj obj(fromjson("{a: {b: [0, [1, 2]]}}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b.1", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1), BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({2U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, PositionalTrailingArrayNotExpandedIfExpandParameterIsFalse) {
    BSONObj obj(fromjson("{a: {b: [0, [1, 2]]}}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = false;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b.1", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << BSON_ARRAY(1 << 2))}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, MidPathEmptyArrayIsConsideredAnArrayComponent) {
    BSONObj obj(fromjson("{a: [{b: []}]}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b.c", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual(std::vector<BSONObj>{}, actualElements);
    assertArrayComponentsAreEqual({0U, 1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, MidPathSingletonArrayIsConsideredAnArrayComponent) {
    BSONObj obj(fromjson("{a: {b: [{c: 3}]}}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b.c", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 3)}, actualElements);
    assertArrayComponentsAreEqual({1U}, actualArrayComponents);
}

TEST(ExtractElementAtPathOrArrayAlongPath, ReturnsArrayEltWithEmptyPathWhenArrayIsAtEndOfPath) {
    BSONObj obj(fromjson("{a: {b: {c: [1, 2, 3]}}}"));
    StringData path("a.b.c");
    const char* pathData = path.rawData();
    auto resultElt = dps::extractElementAtPathOrArrayAlongPath(obj, pathData);
    ASSERT_BSONELT_EQ(resultElt, fromjson("{c: [1, 2, 3]}").firstElement());
    ASSERT(StringData(pathData).empty());
}

TEST(ExtractElementAtPathOrArrayAlongPath, ReturnsArrayEltWithNonEmptyPathForArrayInMiddleOfPath) {
    BSONObj obj(fromjson("{a: {b: [{c: 1}, {c: 2}]}}"));
    StringData path("a.b.c");
    const char* pathData = path.rawData();
    auto resultElt = dps::extractElementAtPathOrArrayAlongPath(obj, pathData);
    ASSERT_BSONELT_EQ(resultElt, fromjson("{b: [{c: 1}, {c: 2}]}").firstElement());
    ASSERT_EQ(StringData(pathData), StringData("c"));
}

TEST(ExtractElementAtPathOrArrayAlongPath, NumericalPathElementNotTreatedAsArrayIndex) {
    BSONObj obj(fromjson("{a: [{'0': 'foo'}]}"));
    StringData path("a.0");
    const char* pathData = path.rawData();
    auto resultElt = dps::extractElementAtPathOrArrayAlongPath(obj, pathData);
    ASSERT_BSONELT_EQ(resultElt, obj.firstElement());
    ASSERT_EQ(StringData(pathData), StringData("0"));
}

TEST(ExtractElementAtPathOrArrayAlongPath, NumericalPathElementTreatedAsFieldNameForNestedObject) {
    BSONObj obj(fromjson("{a: {'0': 'foo'}}"));
    StringData path("a.0");
    const char* pathData = path.rawData();
    auto resultElt = dps::extractElementAtPathOrArrayAlongPath(obj, pathData);
    ASSERT_BSONELT_EQ(resultElt, fromjson("{'0': 'foo'}").firstElement());
    ASSERT(StringData(pathData).empty());
}

}  // namespace
}  // namespace mongo
