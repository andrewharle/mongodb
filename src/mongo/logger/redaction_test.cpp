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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/logger/redaction.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const std::string kRedactionDefaultMask = "###";
const std::string kMsg = "Not initialized";
using BSONStringPair = std::pair<BSONObj, std::string>;

TEST(RedactStringTest, NoRedact) {
    logger::globalLogDomain()->setShouldRedactLogs(false);

    std::string toRedact[] = {"", "abc", "*&$@!_\\\\\\\"*&$@!_\"*&$@!_\"*&$@!_"};
    for (auto s : toRedact) {
        ASSERT_EQ(redact(s), s);
    }
}

TEST(RedactStringTest, BasicStrings) {
    logger::globalLogDomain()->setShouldRedactLogs(true);

    std::string toRedact[] = {"", "abc", "*&$@!_\\\\\\\"*&$@!_\"*&$@!_\"*&$@!_"};
    for (auto s : toRedact) {
        ASSERT_EQ(redact(s), kRedactionDefaultMask);
    }
}

TEST(RedactStatusTest, NoRedact) {
    logger::globalLogDomain()->setShouldRedactLogs(false);
    Status status(ErrorCodes::InternalError, kMsg);
    ASSERT_EQ(redact(status), status.toString());
}

TEST(RedactStatusTest, BasicStatus) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    Status status(ErrorCodes::InternalError, kMsg);
    ASSERT_EQ(redact(status), "InternalError: " + kRedactionDefaultMask);
}

TEST(RedactStatusTest, StatusOK) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    ASSERT_EQ(redact(Status::OK()), "OK");
}

TEST(RedactExceptionTest, NoRedact) {
    logger::globalLogDomain()->setShouldRedactLogs(false);
    ASSERT_THROWS_WITH_CHECK(uasserted(ErrorCodes::InternalError, kMsg),
                             DBException,
                             [](const DBException& ex) { ASSERT_EQ(redact(ex), ex.toString()); });
}

TEST(RedactExceptionTest, BasicException) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    ASSERT_THROWS_WITH_CHECK(
        uasserted(ErrorCodes::InternalError, kMsg), DBException, [](const DBException& ex) {
            ASSERT_EQ(redact(ex), "InternalError ###");
        });
}

TEST(RedactBSONTest, NoRedact) {
    logger::globalLogDomain()->setShouldRedactLogs(false);
    BSONObj obj = BSON("a" << 1);
    ASSERT_EQ(redact(obj), obj.toString());
}

void testBSONCases(std::initializer_list<BSONStringPair> testCases) {
    for (auto m : testCases) {
        ASSERT_EQ(redact(m.first), m.second);
    }
}

TEST(RedactBSONTest, BasicBSON) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    std::vector<BSONStringPair> testCases;

    testBSONCases({BSONStringPair(BSONObj(), "{}"),
                   BSONStringPair(BSON("" << 1), "{ : \"###\" }"),
                   BSONStringPair(BSON("a" << 1), "{ a: \"###\" }"),
                   BSONStringPair(BSON("a" << 1.0), "{ a: \"###\" }"),
                   BSONStringPair(BSON("a"
                                       << "a"),
                                  "{ a: \"###\" }"),
                   BSONStringPair(BSON("a" << 1 << "b"
                                           << "str"),
                                  "{ a: \"###\", b: \"###\" }"),
                   BSONStringPair(BSON("a" << 1 << "a"
                                           << "1"),
                                  "{ a: \"###\", a: \"###\" }")});
}
/*
TEST(RedactBSONTest, NestedBSON) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    std::vector<BSONStringPair> testCases;

    testCases.push_back(BSONStringPair(BSON("a" << BSONObj()), "{ a: {} }"));
    testCases.push_back(BSONStringPair(
        BSON("a" << BSONObj(BSONObj(BSONObj(BSONObj(BSONObj(BSONObj(BSONObj(BSONObj())))))))),
        "{ a: {} }"));
    testCases.push_back(BSONStringPair(BSON("a" << BSON("a" << 1)), "{ a: { a: \"###\" } }"));
    testCases.push_back(BSONStringPair(BSON("a" << BSON("a" << 1 << "b" << 1)),
                                       "{ a: { a: \"###\", b: \"###\" } }"));
    testBSONVector(testCases);
}

TEST(RedactBSONTest, BSONWithArrays) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    std::vector<BSONStringPair> testCases;

    testCases.push_back(BSONStringPair(BSON("a" << BSONArray()), "{ a: [] }"));
    testCases.push_back(
        BSONStringPair(BSON("a" << BSON_ARRAY("abc" << 1)), "{ a: [ \"###\", \"###\" ] }"));
    testCases.push_back(BSONStringPair(BSON("a" << BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1))),
                                       "{ a: [ { a: \"###\" }, { b: \"###\" } ] }"));

    testBSONVector(testCases);
}*/
}  // namespace
}  // namespace mongo
