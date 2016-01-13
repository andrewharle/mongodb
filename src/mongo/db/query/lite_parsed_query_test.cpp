/**
 *    Copyright (C) 2013 MongoDB Inc.
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

/**
 * This file contains tests for mongo/db/query/list_parsed_query.h
 */

#include "mongo/db/query/lite_parsed_query.h"

#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    TEST(LiteParsedQueryTest, InitSortOrder) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 0, 1, 0, BSONObj(), BSONObj(),
                                              fromjson("{a: 1}"), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_OK(result);
    }

    TEST(LiteParsedQueryTest, InitSortOrderString) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 0, 1, 0, BSONObj(), BSONObj(),
                                              fromjson("{a: \"\"}"), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_NOT_OK(result);
    }

    TEST(LiteParsedQueryTest, GetFilter) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 5, 6, 9, BSON( "x" << 5 ), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_OK(result);
        ASSERT_EQUALS(BSON("x" << 5 ), lpq->getFilter());
    }

    TEST(LiteParsedQueryTest, NumToReturn) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 5, 6, 9, BSON( "x" << 5 ), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_OK(result);
        ASSERT_EQUALS(6, lpq->getNumToReturn());
        ASSERT(lpq->wantMore());

        lpq = NULL;
        result = LiteParsedQuery::make("testns", 5, -6, 9, BSON( "x" << 5 ), BSONObj(),
                                       BSONObj(), BSONObj(),
                                       BSONObj(), BSONObj(),
                                       false, // snapshot
                                       false, // explain
                                       &lpq);
        ASSERT_OK(result);
        ASSERT_EQUALS(6, lpq->getNumToReturn());
        ASSERT(!lpq->wantMore());
    }

    TEST(LiteParsedQueryTest, MinFieldsNotPrefixOfMax) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 0, 0, 0, BSONObj(), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              fromjson("{a: 1}"), fromjson("{b: 1}"),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_NOT_OK(result);
    }

    TEST(LiteParsedQueryTest, MinFieldsMoreThanMax) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 0, 0, 0, BSONObj(), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              fromjson("{a: 1, b: 1}"), fromjson("{a: 1}"),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_NOT_OK(result);
    }

    TEST(LiteParsedQueryTest, MinFieldsLessThanMax) {
        LiteParsedQuery* lpq = NULL;
        Status result = LiteParsedQuery::make("testns", 0, 0, 0, BSONObj(), BSONObj(),
                                              BSONObj(), BSONObj(),
                                              fromjson("{a: 1}"), fromjson("{a: 1, b: 1}"),
                                              false, // snapshot
                                              false, // explain
                                              &lpq);
        ASSERT_NOT_OK(result);
    }

    // Helper function which returns the Status of creating a LiteParsedQuery object with the given
    // parameters.
    Status makeLiteParsedQuery(const BSONObj& query, const BSONObj& proj, const BSONObj& sort) {
        LiteParsedQuery* lpqRaw;
        Status result = LiteParsedQuery::make("testns", 0, 0, 0, query, proj, sort, BSONObj(),
                                              BSONObj(), BSONObj(),
                                              false, // snapshot
                                              false, // explain
                                              &lpqRaw);
        if (result.isOK()) {
            boost::scoped_ptr<LiteParsedQuery> lpq(lpqRaw);
        }

        return result;
    }

    //
    // Test compatibility of various projection and sort objects.
    //

    TEST(LiteParsedQueryTest, ValidSortProj) {
        Status result = Status::OK();

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{a: 1}"),
                                     fromjson("{a: 1}"));
        ASSERT_OK(result);

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{a: {$meta: \"textScore\"}}"),
                                     fromjson("{a: {$meta: \"textScore\"}}"));
        ASSERT_OK(result);

    }

    TEST(LiteParsedQueryTest, ForbidNonMetaSortOnFieldWithMetaProject) {
        Status result = Status::OK();

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{a: {$meta: \"textScore\"}}"),
                                     fromjson("{a: 1}"));
        ASSERT_NOT_OK(result);

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{a: {$meta: \"textScore\"}}"),
                                     fromjson("{b: 1}"));
        ASSERT_OK(result);
    }

    TEST(LiteParsedQueryTest, ForbidMetaSortOnFieldWithoutMetaProject) {
        Status result = Status::OK();

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{a: 1}"),
                                     fromjson("{a: {$meta: \"textScore\"}}"));
        ASSERT_NOT_OK(result);

        result = makeLiteParsedQuery(BSONObj(),
                                     fromjson("{b: 1}"),
                                     fromjson("{a: {$meta: \"textScore\"}}"));
        ASSERT_NOT_OK(result);
    }

    //
    // Text meta BSON element validation
    //

    bool isFirstElementTextScoreMeta(const char* sortStr) {
        BSONObj sortObj = fromjson(sortStr);
        BSONElement elt = sortObj.firstElement();
        bool result = LiteParsedQuery::isTextScoreMeta(elt);
        return result;
    }

    // Check validation of $meta expressions
    TEST(LiteParsedQueryTest, IsTextScoreMeta) {
        // Valid textScore meta sort
        ASSERT(isFirstElementTextScoreMeta("{a: {$meta: \"textScore\"}}"));

        // Invalid textScore meta sorts
        ASSERT_FALSE(isFirstElementTextScoreMeta("{a: {$meta: 1}}"));
        ASSERT_FALSE(isFirstElementTextScoreMeta("{a: {$meta: \"image\"}}"));
        ASSERT_FALSE(isFirstElementTextScoreMeta("{a: {$world: \"textScore\"}}"));
        ASSERT_FALSE(isFirstElementTextScoreMeta("{a: {$meta: \"textScore\", b: 1}}"));
    }

    //
    // Sort order validation
    // In a valid sort order, each element satisfies one of:
    // 1. a number with value 1
    // 2. a number with value -1
    // 3. isTextScoreMeta
    //

    TEST(LiteParsedQueryTest, ValidateSortOrder) {
        // Valid sorts
        ASSERT(LiteParsedQuery::isValidSortOrder(fromjson("{}")));
        ASSERT(LiteParsedQuery::isValidSortOrder(fromjson("{a: 1}")));
        ASSERT(LiteParsedQuery::isValidSortOrder(fromjson("{a: -1}")));
        ASSERT(LiteParsedQuery::isValidSortOrder(fromjson("{a: {$meta: \"textScore\"}}")));

        // Invalid sorts
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: 100}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: 0}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: -100}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: Infinity}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: -Infinity}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: true}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: false}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: null}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {}}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {b: 1}}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: []}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: [1, 2, 3]}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: \"\"}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: \"bb\"}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {$meta: 1}}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {$meta: \"image\"}}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {$world: \"textScore\"}}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{a: {$meta: \"textScore\","
                                                                " b: 1}}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{'': 1}")));
        ASSERT_FALSE(LiteParsedQuery::isValidSortOrder(fromjson("{'': -1}")));
    }

}  // namespace
