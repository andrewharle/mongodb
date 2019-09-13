
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

#include "mongo/unittest/unittest.h"

#include "mongo/db/matcher/expression_parser.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"

namespace mongo {

TEST(MatchExpressionParserGeo, WithinBox) {
    BSONObj query = fromjson("{a:{$within:{$box:[{x: 4, y:4},[6,6]]}}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(fromjson("{a: [3,4]}")));
    ASSERT(result.getValue()->matchesBSON(fromjson("{a: [4,4]}")));
    ASSERT(result.getValue()->matchesBSON(fromjson("{a: [5,5]}")));
    ASSERT(result.getValue()->matchesBSON(fromjson("{a: [5,5.1]}")));
    ASSERT(result.getValue()->matchesBSON(fromjson("{a: {x: 5, y:5.1}}")));
}

TEST(MatchExpressionParserGeoNear, ParseNear) {
    BSONObj query = fromjson(
        "{loc:{$near:{$maxDistance:100, "
        "$geometry:{type:\"Point\", coordinates:[0,0]}}}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_TRUE(result.isOK());

    MatchExpression* exp = result.getValue().get();
    ASSERT_EQUALS(MatchExpression::GEO_NEAR, exp->matchType());

    GeoNearMatchExpression* gnexp = static_cast<GeoNearMatchExpression*>(exp);
    ASSERT_EQUALS(gnexp->getData().maxDistance, 100);
}

// $near must be the only field in the expression object.
TEST(MatchExpressionParserGeoNear, ParseNearExtraField) {
    BSONObj query = fromjson(
        "{loc:{$near:{$maxDistance:100, "
        "$geometry:{type:\"Point\", coordinates:[0,0]}}, foo: 1}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                               expCtx,
                                               ExtensionsCallbackNoop(),
                                               MatchExpressionParser::kAllowAllSpecialFeatures)
                      .getStatus());
}

// For $near, $nearSphere, and $geoNear syntax of:
// {
//   $near/$nearSphere/$geoNear: [ <x>, <y> ],
//   $minDistance: <distance in radians>,
//   $maxDistance: <distance in radians>
// }
TEST(MatchExpressionParserGeoNear, ParseValidNear) {
    BSONObj query = fromjson("{loc: {$near: [0,0], $maxDistance: 100, $minDistance: 50}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_TRUE(result.isOK());

    MatchExpression* exp = result.getValue().get();
    ASSERT_EQ(MatchExpression::GEO_NEAR, exp->matchType());

    GeoNearMatchExpression* gnexp = static_cast<GeoNearMatchExpression*>(exp);
    ASSERT_EQ(gnexp->getData().maxDistance, 100);
    ASSERT_EQ(gnexp->getData().minDistance, 50);
}

TEST(MatchExpressionParserGeoNear, ParseInvalidNear) {
    {
        BSONObj query = fromjson("{loc: {$maxDistance: 100, $near: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$minDistance: 100, $near: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$near: [0,0], $maxDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$near: [0,0], $minDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$near: [0,0], $eq: 40}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$eq: 40, $near: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson(
            "{loc: {$near: [0,0], $geoWithin: {$geometry: {type: \"Polygon\", coordinates: []}}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$near: {$foo: 1}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$minDistance: 10}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
}

TEST(MatchExpressionParserGeoNear, ParseValidGeoNear) {
    BSONObj query = fromjson("{loc: {$geoNear: [0,0], $maxDistance: 100, $minDistance: 50}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_TRUE(result.isOK());

    MatchExpression* exp = result.getValue().get();
    ASSERT_EQ(MatchExpression::GEO_NEAR, exp->matchType());

    GeoNearMatchExpression* gnexp = static_cast<GeoNearMatchExpression*>(exp);
    ASSERT_EQ(gnexp->getData().maxDistance, 100);
    ASSERT_EQ(gnexp->getData().minDistance, 50);
}

TEST(MatchExpressionParserGeoNear, ParseInvalidGeoNear) {
    {
        BSONObj query = fromjson("{loc: {$maxDistance: 100, $geoNear: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$minDistance: 100, $geoNear: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$geoNear: [0,0], $eq: 1}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$geoNear: [0,0], $maxDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$geoNear: [0,0], $minDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
}

TEST(MatchExpressionParserGeoNear, ParseValidNearSphere) {
    BSONObj query = fromjson("{loc: {$nearSphere: [0,0], $maxDistance: 100, $minDistance: 50}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_TRUE(result.isOK());

    MatchExpression* exp = result.getValue().get();
    ASSERT_EQ(MatchExpression::GEO_NEAR, exp->matchType());

    GeoNearMatchExpression* gnexp = static_cast<GeoNearMatchExpression*>(exp);
    ASSERT_EQ(gnexp->getData().maxDistance, 100);
    ASSERT_EQ(gnexp->getData().minDistance, 50);
}

TEST(MatchExpressionParserGeoNear, ParseInvalidNearSphere) {
    {
        BSONObj query = fromjson("{loc: {$maxDistance: 100, $nearSphere: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$minDistance: 100, $nearSphere: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$nearSphere: [0,0], $maxDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$nearSphere: [0,0], $minDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$nearSphere: [0,0], $eq: 1}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
}

}  // namespace mongo
