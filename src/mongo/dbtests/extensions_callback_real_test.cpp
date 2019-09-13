
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

#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

//
// $text parsing tests.
//

class ExtensionsCallbackRealTest : public unittest::Test {
public:
    ExtensionsCallbackRealTest() : _nss("unittests.extensions_callback_real_test") {}

    void setUp() final {
        AutoGetOrCreateDb autoDb(&_opCtx, _nss.db(), MODE_X);
        Database* database = autoDb.getDb();
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT(database->createCollection(&_opCtx, _nss.ns()));
            wunit.commit();
        }
    }

    void tearDown() final {
        AutoGetDb autoDb(&_opCtx, _nss.db(), MODE_X);
        Database* database = autoDb.getDb();
        if (!database) {
            return;
        }
        {
            WriteUnitOfWork wunit(&_opCtx);
            static_cast<void>(database->dropCollection(&_opCtx, _nss.ns()));
            wunit.commit();
        }
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    const NamespaceString _nss;
};

TEST_F(ExtensionsCallbackRealTest, TextNoIndex) {
    BSONObj query = fromjson("{$text: {$search:\"awesome\"}}");
    ASSERT_THROWS_CODE(StatusWithMatchExpression result(
                           ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement())),
                       AssertionException,
                       ErrorCodes::IndexNotFound);
}

TEST_F(ExtensionsCallbackRealTest, TextBasic) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $language:\"english\"}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getQuery(), "awesome");
    ASSERT_EQUALS(textExpr->getFTSQuery().getLanguage(), "english");
    ASSERT_EQUALS(textExpr->getFTSQuery().getCaseSensitive(),
                  TextMatchExpressionBase::kCaseSensitiveDefault);
    ASSERT_EQUALS(textExpr->getFTSQuery().getDiacriticSensitive(),
                  TextMatchExpressionBase::kDiacriticSensitiveDefault);
}

TEST_F(ExtensionsCallbackRealTest, TextLanguageError) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $language:\"spanglish\"}}");
    ASSERT_THROWS_CODE(StatusWithMatchExpression result(
                           ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement())),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(ExtensionsCallbackRealTest, TextCaseSensitiveTrue) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $caseSensitive: true}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getCaseSensitive(), true);
}

TEST_F(ExtensionsCallbackRealTest, TextCaseSensitiveFalse) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $caseSensitive: false}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getCaseSensitive(), false);
}

TEST_F(ExtensionsCallbackRealTest, TextCaseSensitiveError) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text:{$search:\"awesome\", $caseSensitive: 0}}");
    StatusWithMatchExpression result =
        ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement());

    ASSERT_NOT_OK(result.getStatus());
}

TEST_F(ExtensionsCallbackRealTest, TextDiacriticSensitiveTrue) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $diacriticSensitive: true}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getDiacriticSensitive(), true);
}

TEST_F(ExtensionsCallbackRealTest, TextDiacriticSensitiveFalse) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $diacriticSensitive: false}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getDiacriticSensitive(), false);
}

TEST_F(ExtensionsCallbackRealTest, TextDiacriticSensitiveError) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text:{$search:\"awesome\", $diacriticSensitive: 0}}");
    StatusWithMatchExpression result =
        ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement());

    ASSERT_NOT_OK(result.getStatus());
}

TEST_F(ExtensionsCallbackRealTest, TextDiacriticSensitiveAndCaseSensitiveTrue) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query =
        fromjson("{$text: {$search:\"awesome\", $diacriticSensitive: true, $caseSensitive: true}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getDiacriticSensitive(), true);
    ASSERT_EQUALS(textExpr->getFTSQuery().getCaseSensitive(), true);
}

//
// $where parsing tests.
//

TEST_F(ExtensionsCallbackRealTest, WhereExpressionsWithSameScopeHaveSameBSONRepresentation) {
    const char code[] = "function(){ return a; }";

    BSONObj query1 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr1 = unittest::assertGet(
        ExtensionsCallbackReal(&_opCtx, &_nss).parseWhere(query1.firstElement()));
    BSONObjBuilder builder1;
    expr1->serialize(&builder1);

    BSONObj query2 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr2 = unittest::assertGet(
        ExtensionsCallbackReal(&_opCtx, &_nss).parseWhere(query2.firstElement()));
    BSONObjBuilder builder2;
    expr2->serialize(&builder2);

    ASSERT_BSONOBJ_EQ(builder1.obj(), builder2.obj());
}

TEST_F(ExtensionsCallbackRealTest,
       WhereExpressionsWithDifferentScopesHaveDifferentBSONRepresentations) {
    const char code[] = "function(){ return a; }";

    BSONObj query1 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr1 = unittest::assertGet(
        ExtensionsCallbackReal(&_opCtx, &_nss).parseWhere(query1.firstElement()));
    BSONObjBuilder builder1;
    expr1->serialize(&builder1);

    BSONObj query2 = BSON("$where" << BSONCodeWScope(code, BSON("a" << false)));
    auto expr2 = unittest::assertGet(
        ExtensionsCallbackReal(&_opCtx, &_nss).parseWhere(query2.firstElement()));
    BSONObjBuilder builder2;
    expr2->serialize(&builder2);

    ASSERT_BSONOBJ_NE(builder1.obj(), builder2.obj());
}

TEST_F(ExtensionsCallbackRealTest, WhereExpressionsWithSameScopeAreEquivalent) {
    const char code[] = "function(){ return a; }";

    BSONObj query1 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr1 = unittest::assertGet(
        ExtensionsCallbackReal(&_opCtx, &_nss).parseWhere(query1.firstElement()));

    BSONObj query2 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr2 = unittest::assertGet(
        ExtensionsCallbackReal(&_opCtx, &_nss).parseWhere(query2.firstElement()));

    ASSERT(expr1->equivalent(expr2.get()));
    ASSERT(expr2->equivalent(expr1.get()));
}

TEST_F(ExtensionsCallbackRealTest, WhereExpressionsWithDifferentScopesAreNotEquivalent) {
    const char code[] = "function(){ return a; }";

    BSONObj query1 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr1 = unittest::assertGet(
        ExtensionsCallbackReal(&_opCtx, &_nss).parseWhere(query1.firstElement()));

    BSONObj query2 = BSON("$where" << BSONCodeWScope(code, BSON("a" << false)));
    auto expr2 = unittest::assertGet(
        ExtensionsCallbackReal(&_opCtx, &_nss).parseWhere(query2.firstElement()));

    ASSERT_FALSE(expr1->equivalent(expr2.get()));
    ASSERT_FALSE(expr2->equivalent(expr1.get()));
}

}  // namespace
}  // namespace mongo
