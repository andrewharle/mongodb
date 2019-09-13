
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

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/stdx/chrono.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::rpc;
using mongo::unittest::assertGet;

ShardingMetadata checkParse(const BSONObj& metadata) {
    return assertGet(ShardingMetadata::readFromMetadata(metadata));
}

const auto kElectionId = OID{"541b1a00e8a23afa832b218e"};
const auto kLastOpTime = repl::OpTime(Timestamp(Seconds{1337}, 800u), 4);

TEST(ShardingMetadata, ReadFromMetadata) {
    {
        auto sm = checkParse(
            BSON("$gleStats" << BSON("lastOpTime" << BSON("ts" << kLastOpTime.getTimestamp() << "t"
                                                               << kLastOpTime.getTerm())
                                                  << "electionId"
                                                  << kElectionId)));
        ASSERT_EQ(sm.getLastElectionId(), kElectionId);
        ASSERT_EQ(sm.getLastOpTime(), kLastOpTime);
    }
    {
        // We don't care about order.
        auto sm = checkParse(
            BSON("$gleStats" << BSON("electionId" << kElectionId << "lastOpTime"
                                                  << BSON("ts" << kLastOpTime.getTimestamp() << "t"
                                                               << kLastOpTime.getTerm()))));

        ASSERT_EQ(sm.getLastElectionId(), kElectionId);
        ASSERT_EQ(sm.getLastOpTime(), kLastOpTime);
    }
}

void checkParseFails(const BSONObj& metadata, ErrorCodes::Error error) {
    auto sm = ShardingMetadata::readFromMetadata(metadata);
    ASSERT_NOT_OK(sm.getStatus());
    ASSERT_EQ(sm.getStatus(), error);
}

TEST(ShardingMetadata, ReadFromInvalidMetadata) {
    { checkParseFails(BSONObj(), ErrorCodes::NoSuchKey); }
    { checkParseFails(BSON("$gleStats" << 1), ErrorCodes::TypeMismatch); }
    { checkParseFails(BSON("$gleStats" << BSONObj()), ErrorCodes::InvalidOptions); }
    {
        checkParseFails(BSON("$gleStats" << BSON("lastOpTime" << 3 << "electionId" << kElectionId)),
                        ErrorCodes::TypeMismatch);
    }
    {
        checkParseFails(
            BSON("$gleStats" << BSON("lastOpTime" << BSON("ts" << kLastOpTime.getTimestamp() << "t"
                                                               << kLastOpTime.getTerm())
                                                  << "electionId"
                                                  << 3)),
            ErrorCodes::TypeMismatch);
    }
    {
        checkParseFails(
            BSON("$gleStats" << BSON("lastOpTime" << kElectionId << "electionId"
                                                  << BSON("ts" << kLastOpTime.getTimestamp() << "t"
                                                               << kLastOpTime.getTerm()))),
            ErrorCodes::TypeMismatch);
    }
    {
        checkParseFails(
            BSON("$gleStats" << BSON("lastOpTime" << BSON("ts" << kLastOpTime.getTimestamp() << "t"
                                                               << kLastOpTime.getTerm())
                                                  << "electionId"
                                                  << kElectionId
                                                  << "extra"
                                                  << "this should not be here")),
            ErrorCodes::InvalidOptions);
    }
}

}  // namespace
