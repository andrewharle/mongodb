
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

#include "mongo/bson/json.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(FindAndModifyRequest, BasicUpdate) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, UpdateWithUpsert) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setUpsert(true);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            upsert: true
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, UpdateWithUpsertFalse) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setUpsert(false);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            upsert: false
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, UpdateWithProjection) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const BSONObj field(BSON("z" << 1));

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setFieldProjection(field);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            fields: { z: 1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, UpdateWithNewTrue) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setShouldReturnNew(true);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            new: true
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, UpdateWithNewFalse) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setShouldReturnNew(false);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            new: false
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, UpdateWithSort) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const BSONObj sort(BSON("z" << -1));

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setSort(sort);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            sort: { z: -1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, UpdateWithCollation) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const BSONObj collation(BSON("locale"
                                 << "en_US"));

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setCollation(collation);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            collation: { locale: 'en_US' }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, UpdateWithArrayFilters) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const std::vector<BSONObj> arrayFilters{BSON("i" << 0)};

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setArrayFilters(arrayFilters);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            arrayFilters: [ { i: 0 } ]
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, UpdateWithWriteConcern) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const WriteConcernOptions writeConcern(2, WriteConcernOptions::SyncMode::FSYNC, 150);

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setWriteConcern(writeConcern);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            writeConcern: { w: 2, fsync: true, wtimeout: 150 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, UpdateWithFullSpec) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const BSONObj sort(BSON("z" << -1));
    const BSONObj collation(BSON("locale"
                                 << "en_US"));
    const std::vector<BSONObj> arrayFilters{BSON("i" << 0)};
    const BSONObj field(BSON("x" << 1 << "y" << 1));
    const WriteConcernOptions writeConcern(2, WriteConcernOptions::SyncMode::FSYNC, 150);

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setFieldProjection(field);
    request.setShouldReturnNew(true);
    request.setSort(sort);
    request.setCollation(collation);
    request.setArrayFilters(arrayFilters);
    request.setWriteConcern(writeConcern);
    request.setUpsert(true);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            upsert: true,
            fields: { x: 1, y: 1 },
            sort: { z: -1 },
            collation: { locale: 'en_US' },
            arrayFilters: [ { i: 0 } ],
            new: true,
            writeConcern: { w: 2, fsync: true, wtimeout: 150 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, BasicRemove) {
    const BSONObj query(BSON("x" << 1));
    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, RemoveWithProjection) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj field(BSON("z" << 1));

    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);
    request.setFieldProjection(field);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            fields: { z: 1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, RemoveWithSort) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj sort(BSON("z" << -1));

    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);
    request.setSort(sort);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            sort: { z: -1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, RemoveWithCollation) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj collation(BSON("locale"
                                 << "en_US"));

    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);
    request.setCollation(collation);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            collation: { locale: 'en_US' }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, RemoveWithWriteConcern) {
    const BSONObj query(BSON("x" << 1));
    const WriteConcernOptions writeConcern(2, WriteConcernOptions::SyncMode::FSYNC, 150);

    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);
    request.setWriteConcern(writeConcern);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            writeConcern: { w: 2, fsync: true, wtimeout: 150 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, RemoveWithFullSpec) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj sort(BSON("z" << -1));
    const BSONObj collation(BSON("locale"
                                 << "en_US"));
    const BSONObj field(BSON("x" << 1 << "y" << 1));
    const WriteConcernOptions writeConcern(2, WriteConcernOptions::SyncMode::FSYNC, 150);

    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);
    request.setFieldProjection(field);
    request.setSort(sort);
    request.setCollation(collation);
    request.setWriteConcern(writeConcern);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            fields: { x: 1, y: 1 },
            sort: { z: -1 },
            collation: { locale: 'en_US' },
            writeConcern: { w: 2, fsync: true, wtimeout: 150 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON());
}

TEST(FindAndModifyRequest, ParseWithUpdateOnlyRequiredFields) {
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            update: { y: 1 }
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_OK(parseStatus.getStatus());

    auto request = parseStatus.getValue();
    ASSERT_EQUALS(NamespaceString("a.b").toString(), request.getNamespaceString().toString());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), request.getQuery());
    ASSERT_BSONOBJ_EQ(BSON("y" << 1), request.getUpdateObj());
    ASSERT_EQUALS(false, request.isUpsert());
    ASSERT_EQUALS(false, request.isRemove());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getFields());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getSort());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getCollation());
    ASSERT_EQUALS(0u, request.getArrayFilters().size());
    ASSERT_EQUALS(false, request.shouldReturnNew());
}

TEST(FindAndModifyRequest, ParseWithUpdateFullSpec) {
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            update: { y: 1 },
            upsert: true,
            fields: { x: 1, y: 1 },
            sort: { z: -1 },
            collation: {locale: 'en_US' },
            arrayFilters: [ { i: 0 } ],
            new: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_OK(parseStatus.getStatus());

    auto request = parseStatus.getValue();
    ASSERT_EQUALS(NamespaceString("a.b").toString(), request.getNamespaceString().toString());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), request.getQuery());
    ASSERT_BSONOBJ_EQ(BSON("y" << 1), request.getUpdateObj());
    ASSERT_EQUALS(true, request.isUpsert());
    ASSERT_EQUALS(false, request.isRemove());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1 << "y" << 1), request.getFields());
    ASSERT_BSONOBJ_EQ(BSON("z" << -1), request.getSort());
    ASSERT_BSONOBJ_EQ(BSON("locale"
                           << "en_US"),
                      request.getCollation());
    ASSERT_EQUALS(1u, request.getArrayFilters().size());
    ASSERT_BSONOBJ_EQ(BSON("i" << 0), request.getArrayFilters()[0]);
    ASSERT_EQUALS(true, request.shouldReturnNew());
}

TEST(FindAndModifyRequest, ParseWithRemoveOnlyRequiredFields) {
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            remove: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_OK(parseStatus.getStatus());

    auto request = parseStatus.getValue();
    ASSERT_EQUALS(NamespaceString("a.b").toString(), request.getNamespaceString().toString());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), request.getQuery());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getUpdateObj());
    ASSERT_EQUALS(false, request.isUpsert());
    ASSERT_EQUALS(true, request.isRemove());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getFields());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getSort());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getCollation());
    ASSERT_EQUALS(false, request.shouldReturnNew());
}

TEST(FindAndModifyRequest, ParseWithRemoveFullSpec) {
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            remove: true,
            fields: { x: 1, y: 1 },
            sort: { z: -1 },
            collation: { locale: 'en_US' },
            new: false
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_OK(parseStatus.getStatus());

    auto request = parseStatus.getValue();
    ASSERT_EQUALS(NamespaceString("a.b").toString(), request.getNamespaceString().toString());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), request.getQuery());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getUpdateObj());
    ASSERT_EQUALS(false, request.isUpsert());
    ASSERT_EQUALS(true, request.isRemove());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1 << "y" << 1), request.getFields());
    ASSERT_BSONOBJ_EQ(BSON("z" << -1), request.getSort());
    ASSERT_BSONOBJ_EQ(BSON("locale"
                           << "en_US"),
                      request.getCollation());
    ASSERT_EQUALS(false, request.shouldReturnNew());
}

TEST(FindAndModifyRequest, ParseWithIncompleteSpec) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 }
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_NOT_OK(parseStatus.getStatus());
}

TEST(FindAndModifyRequest, ParseWithAmbiguousUpdateRemove) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            remove: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_NOT_OK(parseStatus.getStatus());
}

TEST(FindAndModifyRequest, ParseWithRemovePlusUpsert) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            upsert: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_NOT_OK(parseStatus.getStatus());
}

TEST(FindAndModifyRequest, ParseWithRemoveAndReturnNew) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            new: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_NOT_OK(parseStatus.getStatus());
}

TEST(FindAndModifyRequest, ParseWithRemoveAndArrayFilters) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            arrayFilters: [ { i: 0 } ]
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_NOT_OK(parseStatus.getStatus());
}

TEST(FindAndModifyRequest, ParseWithCollationTypeMismatch) {
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            update: { y: 1 },
            collation: 'en_US'
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_EQUALS(parseStatus.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(FindAndModifyRequest, InvalidQueryParameter) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: '{ x: 1 }',
            remove: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_EQ(31160, parseStatus.getStatus().code());
}

TEST(FindAndModifyRequest, InvalidSortParameter) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            sort: 1,
            remove: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_EQ(31174, parseStatus.getStatus().code());
}

TEST(FindAndModifyRequest, InvalidFieldParameter) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            fields: null,
            remove: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_EQ(31175, parseStatus.getStatus().code());
}
}  // unnamed namespace
}  // namespace mongo
