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

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_fixture.h"

namespace {

using namespace mongo;

TEST_F(QueryPlannerTest, Basic2DNonNear) {
    // 2d can answer: within poly, within center, within centersphere, within box.
    // And it can use an index (or not) for each of them.  As such, 2 solns expected.
    addIndex(BSON("a"
                  << "2d"));

    // Polygon
    runQuery(fromjson("{a : { $within: { $polygon : [[0,0], [2,0], [4,0]] } }}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");

    // Center
    runQuery(fromjson("{a : { $within : { $center : [[ 5, 5 ], 7 ] } }}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");

    // Centersphere
    runQuery(fromjson("{a : { $within : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");

    // Within box.
    runQuery(fromjson("{a : {$within: {$box : [[0,0],[9,9]]}}}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");

    // TODO: test that we *don't* annotate for things we shouldn't.
}

TEST_F(QueryPlannerTest, Basic2DSphereCompound) {
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("loc"
                  << "2dsphere"));

    runQuery(fromjson(
        "{loc:{$near:{$geometry:{type:'Point',"
        "coordinates : [-81.513743,28.369947] },"
        " $maxDistance :100}},a: 'mouse'}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {geoNear2dsphere: {loc: '2dsphere'}}}}");
}

TEST_F(QueryPlannerTest, Basic2DCompound) {
    addIndex(BSON("loc"
                  << "2d"
                  << "a" << 1));

    runQuery(fromjson(
        "{ loc: { $geoWithin: { $box : [[0, 0],[10, 10]] } },"
        "a: 'mouse' }"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {loc : '2d', a: 1},"
        "filter: {a: 'mouse'},"
        "bounds: {loc: [],"  // Ignored since complex
        "         a: [['MinKey','MaxKey',true,true]]}"
        "}}}}");
}

TEST_F(QueryPlannerTest, Multikey2DSphereCompound) {
    // true means multikey
    addIndex(BSON("a" << 1 << "b" << 1), true);
    addIndex(BSON("loc"
                  << "2dsphere"),
             true);

    runQuery(fromjson(
        "{loc:{$near:{$geometry:{type:'Point',"
        "coordinates : [-81.513743,28.369947] },"
        " $maxDistance :100}},a: 'mouse'}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {geoNear2dsphere: {loc: '2dsphere'}}}}");
}

TEST_F(QueryPlannerTest, Basic2DSphereNonNear) {
    // 2dsphere can do: within+geometry, intersects+geometry
    addIndex(BSON("a"
                  << "2dsphere"));

    runQuery(fromjson(
        "{a: {$geoIntersects: {$geometry: {type: 'Point',"
        "coordinates: [10.0, 10.0]}}}}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");

    runQuery(fromjson("{a : { $geoWithin : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");

    // TODO: test that we *don't* annotate for things we shouldn't.
}

TEST_F(QueryPlannerTest, Multikey2DSphereNonNear) {
    // 2dsphere can do: within+geometry, intersects+geometry
    // true means multikey
    addIndex(BSON("a"
                  << "2dsphere"),
             true);

    runQuery(fromjson(
        "{a: {$geoIntersects: {$geometry: {type: 'Point',"
        "coordinates: [10.0, 10.0]}}}}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");

    runQuery(fromjson("{a : { $geoWithin : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}"));
    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");

    // TODO: test that we *don't* annotate for things we shouldn't.
}

TEST_F(QueryPlannerTest, Basic2DGeoNear) {
    // Can only do near + old point.
    addIndex(BSON("a"
                  << "2d"));
    runQuery(fromjson("{a: {$near: [0,0], $maxDistance:0.3 }}"));
    assertNumSolutions(1U);
    assertSolutionExists("{geoNear2d: {a: '2d'}}");
}

TEST_F(QueryPlannerTest, Basic2DSphereGeoNear) {
    // Can do nearSphere + old point, near + new point.
    addIndex(BSON("a"
                  << "2dsphere"));

    runQuery(fromjson("{a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{geoNear2dsphere: {a: '2dsphere'}}");

    runQuery(fromjson(
        "{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0,0]},"
        "$maxDistance:100}}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{geoNear2dsphere: {a: '2dsphere'}}");
}

TEST_F(QueryPlannerTest, Multikey2DSphereGeoNear) {
    // Can do nearSphere + old point, near + new point.
    // true means multikey
    addIndex(BSON("a"
                  << "2dsphere"),
             true);

    runQuery(fromjson("{a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists("{geoNear2dsphere: {a: '2dsphere'}}");

    runQuery(fromjson(
        "{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0,0]},"
        "$maxDistance:100}}}"));
    assertNumSolutions(1U);
    assertSolutionExists("{geoNear2dsphere: {a: '2dsphere'}}");
}

TEST_F(QueryPlannerTest, Basic2DSphereGeoNearReverseCompound) {
    addIndex(BSON("x" << 1));
    addIndex(BSON("x" << 1 << "a"
                      << "2dsphere"));
    runQuery(fromjson("{x:1, a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));

    assertNumSolutions(1U);
    assertSolutionExists("{geoNear2dsphere: {x: 1, a: '2dsphere'}}");
}

TEST_F(QueryPlannerTest, Multikey2DSphereGeoNearReverseCompound) {
    addIndex(BSON("x" << 1), true);
    addIndex(BSON("x" << 1 << "a"
                      << "2dsphere"),
             true);
    runQuery(fromjson("{x:1, a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));

    assertNumSolutions(1U);
    assertSolutionExists("{geoNear2dsphere: {x: 1, a: '2dsphere'}}");
}

TEST_F(QueryPlannerTest, NearNoIndex) {
    addIndex(BSON("x" << 1));
    runInvalidQuery(fromjson("{x:1, a: {$nearSphere: [0,0], $maxDistance: 0.31 }}"));
}

TEST_F(QueryPlannerTest, NearEmptyPath) {
    addIndex(BSON(""
                  << "2dsphere"));
    runInvalidQuery(fromjson("{'': {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}"));
}

TEST_F(QueryPlannerTest, TwoDSphereNoGeoPred) {
    addIndex(BSON("x" << 1 << "a"
                      << "2dsphere"));
    runQuery(fromjson("{x:1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1, a: '2dsphere'}}}}}");
}

TEST_F(QueryPlannerTest, TwoDSphereNoGeoPredMultikey) {
    addIndex(BSON("x" << 1 << "a"
                      << "2dsphere"),
             true);
    runQuery(fromjson("{x:1}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {x: 1, a: '2dsphere'}}}}}");
}

// SERVER-14723
TEST_F(QueryPlannerTest, GeoNearMultipleRelevantIndicesButOnlyOneCompatible) {
    addIndex(BSON("a"
                  << "2dsphere"));
    addIndex(BSON("b" << 1 << "a"
                      << "2dsphere"));

    runQuery(fromjson(
        "{a: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0,0]}}},"
        " b: {$exists: false}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {b: {$exists: false}}, node: "
        "{geoNear2dsphere: {a: '2dsphere'}}}}");
}

// SERVER-3984, $or 2d index
TEST_F(QueryPlannerTest, Or2DNonNear) {
    addIndex(BSON("a"
                  << "2d"));
    addIndex(BSON("b"
                  << "2d"));
    runQuery(fromjson(
        "{$or: [ {a : { $within : { $polygon : [[0,0], [2,0], [4,0]] } }},"
        " {b : { $within : { $center : [[ 5, 5 ], 7 ] } }} ]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{or: {nodes: [{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}},"
        "{fetch: {node: {ixscan: {pattern: {b: '2d'}}}}}]}}");
}

// SERVER-3984, $or 2d index
TEST_F(QueryPlannerTest, Or2DSameFieldNonNear) {
    addIndex(BSON("a"
                  << "2d"));
    runQuery(fromjson(
        "{$or: [ {a : { $within : { $polygon : [[0,0], [2,0], [4,0]] } }},"
        " {a : { $within : { $center : [[ 5, 5 ], 7 ] } }} ]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");
}

// SERVER-3984, $or 2dsphere index
TEST_F(QueryPlannerTest, Or2DSphereNonNear) {
    addIndex(BSON("a"
                  << "2dsphere"));
    addIndex(BSON("b"
                  << "2dsphere"));
    runQuery(fromjson(
        "{$or: [ {a: {$geoIntersects: {$geometry: {type: 'Point', coordinates: [10.0, 10.0]}}}},"
        " {b: {$geoWithin: { $centerSphere: [[ 10, 20 ], 0.01 ] } }} ]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{or: {nodes: [{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}},"
        "{fetch: {node: {ixscan: {pattern: {b: '2dsphere'}}}}}]}}");
}

// SERVER-3984, $or 2dsphere index
TEST_F(QueryPlannerTest, Or2DSphereNonNearMultikey) {
    // true means multikey
    addIndex(BSON("a"
                  << "2dsphere"),
             true);
    addIndex(BSON("b"
                  << "2dsphere"),
             true);
    runQuery(fromjson(
        "{$or: [ {a: {$geoIntersects: {$geometry: "
        "{type: 'Point', coordinates: [10.0, 10.0]}}}},"
        " {b: {$geoWithin: { $centerSphere: [[ 10, 20 ], 0.01 ] } }} ]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists(
        "{or: {nodes: "
        "[{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}},"
        "{fetch: {node: {ixscan: {pattern: {b: '2dsphere'}}}}}]}}");
}

TEST_F(QueryPlannerTest, And2DSameFieldNonNear) {
    addIndex(BSON("a"
                  << "2d"));
    runQuery(fromjson(
        "{$and: [ {a : { $within : { $polygon : [[0,0], [2,0], [4,0]] } }},"
        " {a : { $within : { $center : [[ 5, 5 ], 7 ] } }} ]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    // Bounds of the two 2d geo predicates are combined into
    // a single index scan.
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");
}

TEST_F(QueryPlannerTest, And2DWith2DNearSameField) {
    addIndex(BSON("a"
                  << "2d"));
    runQuery(fromjson(
        "{$and: [ {a : { $within : { $polygon : [[0,0], [2,0], [4,0]] } }},"
        " {a : { $near : [ 5, 5 ] } } ]}"));

    // GEO_NEAR must use the index, and GEO predicate becomes a filter.
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: { node : { geoNear2d: {a: '2d'} } } }");
}

TEST_F(QueryPlannerTest, And2DSphereSameFieldNonNear) {
    addIndex(BSON("a"
                  << "2dsphere"));
    runQuery(fromjson(
        "{$and: [ {a: {$geoIntersects: {$geometry: "
        "{type: 'Point', coordinates: [3.0, 1.0]}}}},"
        "  {a: {$geoIntersects: {$geometry: "
        "{type: 'Point', coordinates: [4.0, 1.0]}}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    // Bounds of the two 2dsphere geo predicates are combined into
    // a single index scan.
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");
}

TEST_F(QueryPlannerTest, And2DSphereSameFieldNonNearMultikey) {
    // true means multikey
    addIndex(BSON("a"
                  << "2dsphere"),
             true);
    runQuery(fromjson(
        "{$and: [ {a: {$geoIntersects: {$geometry: "
        "{type: 'Point', coordinates: [3.0, 1.0]}}}},"
        "  {a: {$geoIntersects: {$geometry: "
        "{type: 'Point', coordinates: [4.0, 1.0]}}}}]}"));

    assertNumSolutions(3U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}", 2U);
}

TEST_F(QueryPlannerTest, And2DSphereWithNearSameField) {
    addIndex(BSON("a"
                  << "2dsphere"));
    runQuery(fromjson(
        "{$and: [{a: {$geoIntersects: {$geometry: "
        "{type: 'Point', coordinates: [3.0, 1.0]}}}},"
        "{a: {$near: {$geometry: "
        "{type: 'Point', coordinates: [10.0, 10.0]}}}}]}"));

    // GEO_NEAR must use the index, and GEO predicate becomes a filter.
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {geoNear2dsphere: {a: '2dsphere'}}}}");
}

TEST_F(QueryPlannerTest, And2DSphereWithNearSameFieldMultikey) {
    // true means multikey
    addIndex(BSON("a"
                  << "2dsphere"),
             true);
    runQuery(fromjson(
        "{$and: [{a: {$geoIntersects: {$geometry: "
        "{type: 'Point', coordinates: [3.0, 1.0]}}}},"
        "{a: {$near: {$geometry: "
        "{type: 'Point', coordinates: [10.0, 10.0]}}}}]}"));

    // GEO_NEAR must use the index, and GEO predicate becomes a filter.
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {geoNear2dsphere: {a: '2dsphere'}}}}");
}

TEST_F(QueryPlannerTest, Or2DSphereSameFieldNonNear) {
    addIndex(BSON("a"
                  << "2dsphere"));
    runQuery(fromjson(
        "{$or: [ {a: {$geoIntersects: {$geometry: "
        "{type: 'Point', coordinates: [3.0, 1.0]}}}},"
        "  {a: {$geoIntersects: {$geometry: "
        "{type: 'Point', coordinates: [4.0, 1.0]}}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");
}

TEST_F(QueryPlannerTest, Or2DSphereSameFieldNonNearMultikey) {
    // true means multikey
    addIndex(BSON("a"
                  << "2dsphere"),
             true);
    runQuery(fromjson(
        "{$or: [ {a: {$geoIntersects: {$geometry: "
        "{type: 'Point', coordinates: [3.0, 1.0]}}}},"
        "  {a: {$geoIntersects: {$geometry: "
        "{type: 'Point', coordinates: [4.0, 1.0]}}}}]}"));

    assertNumSolutions(2U);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");
}

TEST_F(QueryPlannerTest, CompoundMultikey2DSphereNear) {
    // true means multikey
    addIndex(BSON("a" << 1 << "b"
                      << "2dsphere"),
             true);
    runQuery(fromjson(
        "{a: {$gte: 0}, b: {$near: {$geometry: "
        "{type: 'Point', coordinates: [2, 2]}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{geoNear2dsphere: {a: 1, b: '2dsphere'}}");
}

TEST_F(QueryPlannerTest, CompoundMultikey2DSphereNearFetchRequired) {
    // true means multikey
    addIndex(BSON("a" << 1 << "b"
                      << "2dsphere"),
             true);
    runQuery(fromjson(
        "{a: {$gte: 0, $lt: 5}, b: {$near: {$geometry: "
        "{type: 'Point', coordinates: [2, 2]}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a:{$gte:0}}, node: "
        "{geoNear2dsphere: {a: 1, b: '2dsphere'}}}}");
}

TEST_F(QueryPlannerTest, CompoundMultikey2DSphereNearMultipleIndices) {
    // true means multikey
    addIndex(BSON("a" << 1 << "b"
                      << "2dsphere"),
             true);
    addIndex(BSON("c" << 1 << "b"
                      << "2dsphere"),
             true);
    runQuery(fromjson(
        "{a: {$gte: 0}, c: 3, b: {$near: {$geometry: "
        "{type: 'Point', coordinates: [2, 2]}}}}"));

    assertNumSolutions(2U);
    assertSolutionExists(
        "{fetch: {filter: {c:3}, node: "
        "{geoNear2dsphere: {a: 1, b: '2dsphere'}}}}");
    assertSolutionExists(
        "{fetch: {filter: {a:{$gte:0}}, node: "
        "{geoNear2dsphere: {c: 1, b: '2dsphere'}}}}");
}

TEST_F(QueryPlannerTest, CompoundMultikey2DSphereNearMultipleLeadingFields) {
    // true means multikey
    addIndex(BSON("a" << 1 << "b" << 1 << "c"
                      << "2dsphere"),
             true);
    runQuery(fromjson(
        "{a: {$lt: 5, $gt: 1}, b: 6, c: {$near: {$geometry: "
        "{type: 'Point', coordinates: [2, 2]}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a:{$gt:1}}, node: "
        "{geoNear2dsphere: {a: 1, b: 1, c: '2dsphere'}}}}");
}

TEST_F(QueryPlannerTest, CompoundMultikey2DSphereNearMultipleGeoPreds) {
    // true means multikey
    addIndex(BSON("a" << 1 << "b" << 1 << "c"
                      << "2dsphere"),
             true);
    runQuery(fromjson(
        "{a: 1, b: 6, $and: ["
        "{c: {$near: {$geometry: {type: 'Point', coordinates: [2, 2]}}}},"
        "{c: {$geoWithin: {$box: [ [1, 1], [3, 3] ] } } } ] }"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {geoNear2dsphere: {a:1, b:1, c:'2dsphere'}}}}");
}

TEST_F(QueryPlannerTest, CompoundMultikey2DSphereNearCompoundTest) {
    // true means multikey
    addIndex(BSON("a" << 1 << "b"
                      << "2dsphere"
                      << "c" << 1 << "d" << 1),
             true);
    runQuery(fromjson(
        "{a: {$gte: 0}, c: {$gte: 0, $lt: 4}, d: {$gt: 1, $lt: 5},"
        "b: {$near: {$geometry: "
        "{type: 'Point', coordinates: [2, 2]}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {d:{$gt:1},c:{$gte:0}}, node: "
        "{geoNear2dsphere: {a: 1, b: '2dsphere', c: 1, d: 1}}}}");
}

TEST_F(QueryPlannerTest, CompoundMultikey2DNear) {
    // true means multikey
    addIndex(BSON("a"
                  << "2d"
                  << "b" << 1),
             true);
    runQuery(fromjson("{a: {$near: [0, 0]}, b: {$gte: 0}}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: { filter : {b:{$gte: 0}}, node: "
        "{geoNear2d: {a: '2d', b: 1} } } }");
}

// SERVER-9257
TEST_F(QueryPlannerTest, CompoundGeoNoGeoPredicate) {
    addIndex(BSON("creationDate" << 1 << "foo.bar"
                                 << "2dsphere"));
    runQuerySortProj(
        fromjson("{creationDate: { $gt: 7}}"), fromjson("{creationDate: 1}"), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {creationDate: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {creationDate: 1, 'foo.bar': '2dsphere'}}}}}");
}

// SERVER-9257
TEST_F(QueryPlannerTest, CompoundGeoNoGeoPredicateMultikey) {
    // true means multikey
    addIndex(BSON("creationDate" << 1 << "foo.bar"
                                 << "2dsphere"),
             true);
    runQuerySortProj(
        fromjson("{creationDate: { $gt: 7}}"), fromjson("{creationDate: 1}"), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {creationDate: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1}}}}}}");
    assertSolutionExists(
        "{fetch: {node: {ixscan: {pattern: {creationDate: 1, 'foo.bar': '2dsphere'}}}}}");
}

// Test that a 2dsphere index can satisfy a whole index scan solution if the query has a GEO
// predicate on at least one of the indexed geo fields.
// Currently fails.  Tracked by SERVER-10801.
/*
TEST_F(QueryPlannerTest, SortOnGeoQuery) {
    addIndex(BSON("timestamp" << -1 << "position" << "2dsphere"));
    BSONObj query = fromjson("{position: {$geoWithin: {$geometry: {type: \"Polygon\", coordinates:
    [[[1, 1], [1, 90], [180, 90], [180, 1], [1, 1]]]}}}}"); BSONObj sort = fromjson("{timestamp:
    -1}");
    runQuerySortProj(query, sort, BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{sort: {pattern: {timestamp: -1}, limit: 0, "
                            "node: {cscan: {dir: 1}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {timestamp: -1, position:
    '2dsphere'}}}}}"); }

TEST_F(QueryPlannerTest, SortOnGeoQueryMultikey) {
    // true means multikey
    addIndex(BSON("timestamp" << -1 << "position" << "2dsphere"), true);
    BSONObj query = fromjson("{position: {$geoWithin: {$geometry: {type: \"Polygon\", "
        "coordinates: [[[1, 1], [1, 90], [180, 90], [180, 1], [1, 1]]]}}}}");
    BSONObj sort = fromjson("{timestamp: -1}");
    runQuerySortProj(query, sort, BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists("{sort: {pattern: {timestamp: -1}, limit: 0, "
                            "node: {cscan: {dir: 1}}}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: "
                            "{timestamp: -1, position: '2dsphere'}}}}}");
}
*/


//
// Sort
//

TEST_F(QueryPlannerTest, CantUseNonCompoundGeoIndexToProvideSort) {
    addIndex(BSON("x"
                  << "2dsphere"));
    runQuerySortProj(BSONObj(), BSON("x" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1, filter: {}}}}}}}");
}

TEST_F(QueryPlannerTest, CantUseNonCompoundGeoIndexToProvideSortWithIndexablePred) {
    addIndex(BSON("x"
                  << "2dsphere"));
    runQuerySortProj(fromjson(
                         "{x: {$geoIntersects: {$geometry: {type: 'Point',"
                         "                                  coordinates: [0, 0]}}}}"),
                     BSON("x" << 1),
                     BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{fetch: {node: {ixscan: {pattern: {x: '2dsphere'}}}}}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
}

TEST_F(QueryPlannerTest, CantUseCompoundGeoIndexToProvideSortIfNoGeoPred) {
    addIndex(BSON("x" << 1 << "y"
                      << "2dsphere"));
    runQuerySortProj(BSONObj(), BSON("x" << 1), BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, node: {sortKeyGen: "
        "{node: {cscan: {dir: 1, filter: {}}}}}}}");
}

TEST_F(QueryPlannerTest, CanUseCompoundGeoIndexToProvideSortWithGeoPred) {
    addIndex(BSON("x" << 1 << "y"
                      << "2dsphere"));
    runQuerySortProj(fromjson(
                         "{x: 1, y: {$geoIntersects: {$geometry: {type: 'Point',"
                         "                                        coordinates: [0, 0]}}}}"),
                     BSON("x" << 1),
                     BSONObj());

    ASSERT_EQUALS(getNumSolutions(), 2U);
    assertSolutionExists(
        "{fetch: {node: "
        "{ixscan: {pattern: {x: 1, y: '2dsphere'}}}}}");
    assertSolutionExists(
        "{sort: {pattern: {x: 1}, limit: 0, node: {sortKeyGen: {node: "
        "{cscan: {dir: 1}}}}}}");
}

//
// Negation
//

//
// 2D geo negation
// The filter b != 1 is embedded in the geoNear2d node.
// Can only do near + old point.
//
TEST_F(QueryPlannerTest, Negation2DGeoNear) {
    addIndex(BSON("a"
                  << "2d"));
    runQuery(fromjson("{$and: [{a: {$near: [0, 0], $maxDistance: 0.3}}, {b: {$ne: 1}}]}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: { geoNear2d: {a: '2d'} } } }");
}

//
// 2DSphere geo negation
// Filter is embedded in a separate fetch node.
//
TEST_F(QueryPlannerTest, Negation2DSphereGeoNear) {
    // Can do nearSphere + old point, near + new point.
    addIndex(BSON("a"
                  << "2dsphere"));

    runQuery(fromjson(
        "{$and: [{a: {$nearSphere: [0,0], $maxDistance: 0.31}}, "
        "{b: {$ne: 1}}]}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {geoNear2dsphere: {a: '2dsphere'}}}}");

    runQuery(fromjson(
        "{$and: [{a: {$geoNear: {$geometry: {type: 'Point', "
        "coordinates: [0, 0]},"
        "$maxDistance: 100}}},"
        "{b: {$ne: 1}}]}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {geoNear2dsphere: {a: '2dsphere'}}}}");
}

//
// 2DSphere geo negation
// Filter is embedded in a separate fetch node.
//
TEST_F(QueryPlannerTest, Negation2DSphereGeoNearMultikey) {
    // Can do nearSphere + old point, near + new point.
    // true means multikey
    addIndex(BSON("a"
                  << "2dsphere"),
             true);

    runQuery(fromjson(
        "{$and: [{a: {$nearSphere: [0,0], $maxDistance: 0.31}}, "
        "{b: {$ne: 1}}]}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {geoNear2dsphere: {a: '2dsphere'}}}}");

    runQuery(fromjson(
        "{$and: [{a: {$geoNear: {$geometry: {type: 'Point', "
        "coordinates: [0, 0]},"
        "$maxDistance: 100}}},"
        "{b: {$ne: 1}}]}"));
    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {geoNear2dsphere: {a: '2dsphere'}}}}");
}

//
// 2dsphere V2 sparse indices, SERVER-9639
//

// Basic usage of a sparse 2dsphere index.  V1 ignores the sparse field.  We can use any prefix
// of the index as every document is indexed.
TEST_F(QueryPlannerTest, TwoDSphereSparseV1) {
    // Create a V1 index.
    addIndex(BSON("nonGeo" << 1 << "geo"
                           << "2dsphere"),
             BSON("2dsphereIndexVersion" << 1));

    // Can use the index for this.
    runQuery(fromjson("{nonGeo: 7}"));
    assertNumSolutions(2);
    assertSolutionExists("{cscan: {dir: 1}}");
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {nonGeo: 1, geo: '2dsphere'}}}}}");
}

// V2 is "geo sparse" and removes the nonGeo assignment.
TEST_F(QueryPlannerTest, TwoDSphereSparseV2CantUse) {
    // Create a V2 index.
    addIndex(BSON("nonGeo" << 1 << "geo"
                           << "2dsphere"),
             BSON("2dsphereIndexVersion" << 2));

    // Can't use the index prefix here as it's a V2 index and we have no geo pred.
    runQuery(fromjson("{nonGeo: 7}"));
    assertNumSolutions(1);
    assertSolutionExists("{cscan: {dir: 1}}");
}

TEST_F(QueryPlannerTest, TwoDSphereSparseOnePred) {
    // Create a V2 index.
    addIndex(BSON("geo"
                  << "2dsphere"),
             BSON("2dsphereIndexVersion" << 2));

    // We can use the index here as we have a geo pred.
    runQuery(fromjson("{geo : { $geoWithin : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}}"));
    assertNumSolutions(2);
    assertSolutionExists("{cscan: {dir: 1}}");
}

// V2 is geo-sparse and the planner removes the nonGeo assignment when there's no geo pred
TEST_F(QueryPlannerTest, TwoDSphereSparseV2TwoPreds) {
    addIndex(BSON("nonGeo" << 1 << "geo"
                           << "2dsphere"
                           << "geo2"
                           << "2dsphere"),
             BSON("2dsphereIndexVersion" << 2));

    // Non-geo preds can only use a collscan.
    runQuery(fromjson("{nonGeo: 7}"));
    assertNumSolutions(1);
    assertSolutionExists("{cscan: {dir: 1}}");

    // One geo pred so we can use the index.
    runQuery(
        fromjson("{nonGeo: 7, geo : { $geoWithin : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}}"));
    ASSERT_EQUALS(getNumSolutions(), 2U);

    // Two geo preds, so we can use the index still.
    runQuery(fromjson(
        "{nonGeo: 7, geo : { $geoWithin : { $centerSphere : [[ 10, 20 ], 0.01 ] }},"
        " geo2 : { $geoWithin : { $centerSphere : [[ 10, 20 ], 0.01 ] }}}"));
    ASSERT_EQUALS(getNumSolutions(), 2U);
}

TEST_F(QueryPlannerTest, TwoDNearCompound) {
    addIndex(BSON("geo"
                  << "2dsphere"
                  << "nongeo" << 1),
             BSON("2dsphereIndexVersion" << 2));
    runQuery(fromjson("{geo: {$nearSphere: [-71.34895, 42.46037]}}"));
    ASSERT_EQUALS(getNumSolutions(), 1U);
}

TEST_F(QueryPlannerTest, TwoDSphereSparseV2BelowOr) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;

    addIndex(BSON("geo1"
                  << "2dsphere"
                  << "a" << 1 << "b" << 1),
             BSON("2dsphereIndexVersion" << 2));
    addIndex(BSON("geo2"
                  << "2dsphere"
                  << "a" << 1 << "b" << 1),
             BSON("2dsphereIndexVersion" << 2));

    runQuery(fromjson(
        "{a: 4, b: 5, $or: ["
        "{geo1: {$geoWithin: {$centerSphere: [[10, 20], 0.01]}}},"
        "{geo2: {$geoWithin: {$centerSphere: [[10, 20], 0.01]}}}]}"));

    assertNumSolutions(1U);
    assertSolutionExists(
        "{fetch: {filter: {a: 4, b: 5}, node: {or: {nodes: ["
        "{fetch: {node: {ixscan: {pattern: {geo1:'2dsphere',a:1,b:1}}}}},"
        "{fetch: {node: {ixscan: {pattern: {geo2:'2dsphere',a:1,b:1}}}}}"
        "]}}}}");
}

TEST_F(QueryPlannerTest, TwoDSphereSparseV2BelowElemMatch) {
    params.options = QueryPlannerParams::NO_TABLE_SCAN;
    addIndex(BSON("a.b"
                  << "2dsphere"
                  << "a.c" << 1),
             BSON("2dsphereIndexVersion" << 2));

    runQuery(fromjson(
        "{a: {$elemMatch: {b: {$geoWithin: {$centerSphere: [[10,20], 0.01]}},"
        "c: {$gt: 3}}}}"));

    assertNumSolutions(1U);
    assertSolutionExists("{fetch: {node: {ixscan: {pattern: {'a.b': '2dsphere', 'a.c': 1}}}}}");
}

}  // namespace
