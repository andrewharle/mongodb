
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

class DocumentSourceTruncateSort : public ServiceContextTest {};

TEST_F(DocumentSourceTruncateSort, SortTruncatesNormalField) {
    SimpleBSONObjComparator bsonComparator{};
    BSONObj sortKey = BSON("a" << 1 << "b" << 1 << "c" << 1);
    auto truncated =
        DocumentSource::truncateSortSet(bsonComparator.makeBSONObjSet({sortKey}), {"b"});
    ASSERT_EQUALS(truncated.size(), 1U);
    ASSERT_EQUALS(truncated.count(BSON("a" << 1)), 1U);
}

TEST_F(DocumentSourceTruncateSort, SortTruncatesOnSubfield) {
    SimpleBSONObjComparator bsonComparator{};
    BSONObj sortKey = BSON("a" << 1 << "b.c" << 1 << "d" << 1);
    auto truncated =
        DocumentSource::truncateSortSet(bsonComparator.makeBSONObjSet({sortKey}), {"b"});
    ASSERT_EQUALS(truncated.size(), 1U);
    ASSERT_EQUALS(truncated.count(BSON("a" << 1)), 1U);
}

TEST_F(DocumentSourceTruncateSort, SortDoesNotTruncateOnParent) {
    SimpleBSONObjComparator bsonComparator{};
    BSONObj sortKey = BSON("a" << 1 << "b" << 1 << "d" << 1);
    auto truncated =
        DocumentSource::truncateSortSet(bsonComparator.makeBSONObjSet({sortKey}), {"b.c"});
    ASSERT_EQUALS(truncated.size(), 1U);
    ASSERT_EQUALS(truncated.count(BSON("a" << 1 << "b" << 1 << "d" << 1)), 1U);
}

TEST_F(DocumentSourceTruncateSort, TruncateSortDedupsSortCorrectly) {
    SimpleBSONObjComparator bsonComparator{};
    BSONObj sortKeyOne = BSON("a" << 1 << "b" << 1);
    BSONObj sortKeyTwo = BSON("a" << 1);
    auto truncated = DocumentSource::truncateSortSet(
        bsonComparator.makeBSONObjSet({sortKeyOne, sortKeyTwo}), {"b"});
    ASSERT_EQUALS(truncated.size(), 1U);
    ASSERT_EQUALS(truncated.count(BSON("a" << 1)), 1U);
}

}  // namespace
}  // namespace mongo
