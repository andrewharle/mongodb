/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/client/read_preference.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

using unittest::assertGet;

const Seconds minMaxStalenessSeconds(
    durationCount<Seconds>(ReadPreferenceSetting::kMinimalMaxStalenessValue));

void checkParse(const BSONObj& rpsObj, const ReadPreferenceSetting& expected) {
    const auto rps = assertGet(ReadPreferenceSetting::fromBSON(rpsObj));
    if (!rps.equals(expected)) {
        FAIL(str::stream() << "Expected " << expected.toString() << " does not match actual "
                           << rps.toString());
    }
}

TEST(ReadPreferenceSetting, ParseValid) {
    checkParse(BSON("mode"
                    << "primary"),
               ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

    // Check that primary with wildcard tags is accepted for backwards compatibility, but
    // that the tags are parsed as the empty TagSet.
    checkParse(BSON("mode"
                    << "primary"
                    << "tags"
                    << BSON_ARRAY(BSONObj())),
               ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

    checkParse(BSON("mode"
                    << "secondaryPreferred"
                    << "tags"
                    << BSON_ARRAY(BSON("dc"
                                       << "ny"))),
               ReadPreferenceSetting(ReadPreference::SecondaryPreferred,
                                     TagSet(BSON_ARRAY(BSON("dc"
                                                            << "ny")))));
    checkParse(BSON("mode"
                    << "secondary"
                    << "maxStalenessSeconds"
                    << minMaxStalenessSeconds.count()),
               ReadPreferenceSetting(ReadPreference::SecondaryOnly, minMaxStalenessSeconds));

    checkParse(BSON("mode"
                    << "secondary"
                    << "maxStalenessSeconds"
                    << 0),
               ReadPreferenceSetting(ReadPreference::SecondaryOnly, Milliseconds(0)));

    checkParse(BSON("mode"
                    << "secondary"
                    << "maxStalenessSeconds"
                    << 61LL),
               ReadPreferenceSetting(ReadPreference::SecondaryOnly, Milliseconds(61000)));

    checkParse(BSON("mode"
                    << "secondary"
                    << "maxStalenessSeconds"
                    << 63.46),
               ReadPreferenceSetting(ReadPreference::SecondaryOnly, Milliseconds(63460)));

    checkParse(BSON("mode"
                    << "secondary"
                    << "tags"
                    << BSON_ARRAY(BSON("dc"
                                       << "ny"))
                    << "maxStalenessSeconds"
                    << minMaxStalenessSeconds.count()),
               ReadPreferenceSetting(ReadPreference::SecondaryOnly,
                                     TagSet(BSON_ARRAY(BSON("dc"
                                                            << "ny"))),
                                     minMaxStalenessSeconds));
}

void checkParseFails(const BSONObj& rpsObj) {
    auto swRps = ReadPreferenceSetting::fromBSON(rpsObj);
    ASSERT_NOT_OK(swRps.getStatus());
}

void checkParseFailsWithError(const BSONObj& rpsObj, ErrorCodes::Error error) {
    auto swRps = ReadPreferenceSetting::fromBSON(rpsObj);
    ASSERT_NOT_OK(swRps.getStatus());
    ASSERT_EQUALS(swRps.getStatus().code(), error);
}

TEST(ReadPreferenceSetting, NonEquality) {
    auto tagSet = TagSet(BSON_ARRAY(BSON("dc"
                                         << "ca")
                                    << BSON("foo"
                                            << "bar")));
    auto rps = ReadPreferenceSetting(ReadPreference::Nearest, tagSet, minMaxStalenessSeconds);

    auto unexpected1 = ReadPreferenceSetting(
        ReadPreference::Nearest, TagSet::primaryOnly(), minMaxStalenessSeconds);
    ASSERT_FALSE(rps.equals(unexpected1));

    auto unexpected2 = ReadPreferenceSetting(
        ReadPreference::Nearest, tagSet, Seconds(minMaxStalenessSeconds.count() + 1));
    ASSERT_FALSE(rps.equals(unexpected2));
}

TEST(ReadPreferenceSetting, ParseInvalid) {
    // mode primary can not have tags
    checkParseFails(BSON("mode"
                         << "primary"
                         << "tags"
                         << BSON_ARRAY(BSON("foo"
                                            << "bar"))));
    // bad mode
    checkParseFails(BSON("mode"
                         << "khalesi"));

    // no mode
    checkParseFails(BSON("foo"
                         << "bar"));

    // tags not an array
    checkParseFails(BSON("mode"
                         << "nearest"
                         << "tags"
                         << "bad"));

    // maxStalenessSeconds is negative
    checkParseFailsWithError(BSON("mode"
                                  << "secondary"
                                  << "maxStalenessSeconds"
                                  << -1),
                             ErrorCodes::BadValue);

    // maxStalenessSeconds is NaN
    checkParseFailsWithError(BSON("mode"
                                  << "secondary"
                                  << "maxStalenessSeconds"
                                  << "ONE"),
                             ErrorCodes::TypeMismatch);

    // maxStalenessSeconds and primary
    checkParseFails(BSON("mode"
                         << "primary"
                         << "maxStalenessSeconds"
                         << minMaxStalenessSeconds.count()));

    // maxStalenessSeconds is less than min
    checkParseFailsWithError(BSON("mode"
                                  << "primary"
                                  << "maxStalenessSeconds"
                                  << minMaxStalenessSeconds.count() - 1),
                             ErrorCodes::MaxStalenessOutOfRange);

    // maxStalenessSeconds is greater than max type value for milliseconds
    checkParseFailsWithError(BSON("mode"
                                  << "secondary"
                                  << "maxStalenessSeconds"
                                  << Milliseconds::max().count()),
                             ErrorCodes::MaxStalenessOutOfRange);
}

void checkRoundtrip(const ReadPreferenceSetting& rps) {
    auto parsed = assertGet(ReadPreferenceSetting::fromBSON(rps.toBSON()));
    ASSERT_TRUE(parsed.equals(rps));
}

TEST(ReadPreferenceSetting, Roundtrip) {
    checkRoundtrip(ReadPreferenceSetting(ReadPreference::Nearest,
                                         TagSet(BSON_ARRAY(BSON("dc"
                                                                << "ca")
                                                           << BSON("foo"
                                                                   << "bar")))));
    checkRoundtrip(ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    checkRoundtrip(ReadPreferenceSetting(ReadPreference::PrimaryPreferred, TagSet()));

    checkRoundtrip(ReadPreferenceSetting(ReadPreference::SecondaryOnly,
                                         TagSet(BSON_ARRAY(BSON("dc"
                                                                << "ca"
                                                                << "rack"
                                                                << "bar")))));

    checkRoundtrip(ReadPreferenceSetting(ReadPreference::Nearest,
                                         TagSet(BSON_ARRAY(BSON("dc"
                                                                << "ca")
                                                           << BSON("foo"
                                                                   << "bar"))),
                                         minMaxStalenessSeconds));

    checkRoundtrip(ReadPreferenceSetting(ReadPreference::Nearest,
                                         TagSet(BSON_ARRAY(BSON("dc"
                                                                << "ca")
                                                           << BSON("foo"
                                                                   << "bar"))),
                                         Milliseconds(63246)));
}

}  // namespace
}  // namespace mongo
