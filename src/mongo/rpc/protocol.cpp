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

#include "mongo/rpc/protocol.h"

#include <algorithm>
#include <iterator>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/wire_version.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace rpc {

namespace {

/**
 * Protocols supported by order of preference.
 */
const Protocol kPreferredProtos[] = {Protocol::kOpCommandV1, Protocol::kOpQuery};

const char kNone[] = "none";
const char kOpQueryOnly[] = "opQueryOnly";
const char kOpCommandOnly[] = "opCommandOnly";
const char kAll[] = "all";

}  // namespace

StatusWith<Protocol> negotiate(ProtocolSet fst, ProtocolSet snd) {
    using std::begin;
    using std::end;

    ProtocolSet common = fst & snd;

    auto it = std::find_if(begin(kPreferredProtos), end(kPreferredProtos), [common](Protocol p) {
        return common & static_cast<ProtocolSet>(p);
    });

    if (it == end(kPreferredProtos)) {
        return Status(ErrorCodes::RPCProtocolNegotiationFailed, "No common protocol found.");
    }
    return *it;
}

StatusWith<StringData> toString(ProtocolSet protocols) {
    switch (protocols) {
        case supports::kNone:
            return StringData(kNone);
        case supports::kOpQueryOnly:
            return StringData(kOpQueryOnly);
        case supports::kOpCommandOnly:
            return StringData(kOpCommandOnly);
        case supports::kAll:
            return StringData(kAll);
        default:
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Can not convert ProtocolSet " << protocols
                                        << " to a string, only the predefined ProtocolSet "
                                        << "constants 'none' (0x0), 'opQueryOnly' (0x1), "
                                        << "'opCommandOnly' (0x2), and 'all' (0x3) are supported.");
    }
}

StatusWith<ProtocolSet> parseProtocolSet(StringData repr) {
    if (repr == kNone) {
        return supports::kNone;
    } else if (repr == kOpQueryOnly) {
        return supports::kOpQueryOnly;
    } else if (repr == kOpCommandOnly) {
        return supports::kOpCommandOnly;
    } else if (repr == kAll) {
        return supports::kAll;
    }
    return Status(ErrorCodes::BadValue,
                  str::stream() << "Can not parse a ProtocolSet from " << repr
                                << " only the predefined ProtocolSet constants "
                                << "'none' (0x0), 'opQueryOnly' (0x1), 'opCommandOnly' (0x2), "
                                << "and 'all' (0x3) are supported.");
}

StatusWith<ProtocolSetAndWireVersionInfo> parseProtocolSetFromIsMasterReply(
    const BSONObj& isMasterReply) {
    long long maxWireVersion;
    auto maxWireExtractStatus =
        bsonExtractIntegerField(isMasterReply, "maxWireVersion", &maxWireVersion);

    long long minWireVersion;
    auto minWireExtractStatus =
        bsonExtractIntegerField(isMasterReply, "minWireVersion", &minWireVersion);

    // MongoDB 2.4 and earlier do not have maxWireVersion/minWireVersion in their 'isMaster' replies
    if ((maxWireExtractStatus == minWireExtractStatus) &&
        (maxWireExtractStatus == ErrorCodes::NoSuchKey)) {
        return {{supports::kOpQueryOnly, {0, 0}}};
    } else if (!maxWireExtractStatus.isOK()) {
        return maxWireExtractStatus;
    } else if (!minWireExtractStatus.isOK()) {
        return minWireExtractStatus;
    }

    bool isMongos = false;

    std::string msgField;
    auto msgFieldExtractStatus = bsonExtractStringField(isMasterReply, "msg", &msgField);

    if (msgFieldExtractStatus == ErrorCodes::NoSuchKey) {
        isMongos = false;
    } else if (!msgFieldExtractStatus.isOK()) {
        return msgFieldExtractStatus;
    } else {
        isMongos = (msgField == "isdbgrid");
    }

    if (minWireVersion < 0 || maxWireVersion < 0 ||
        minWireVersion >= std::numeric_limits<int>::max() ||
        maxWireVersion >= std::numeric_limits<int>::max()) {
        return Status(ErrorCodes::IncompatibleServerVersion,
                      str::stream() << "Server min and max wire version have invalid values ("
                                    << minWireVersion
                                    << ","
                                    << maxWireVersion
                                    << ")");
    }

    WireVersionInfo version{static_cast<int>(minWireVersion), static_cast<int>(maxWireVersion)};

    return {{(!isMongos && supportsWireVersionForOpCommandInMongod(version))
                 ? supports::kAll
                 : supports::kOpQueryOnly,
             version}};
}

bool supportsWireVersionForOpCommandInMongod(const WireVersionInfo version) {
    // FIND_COMMAND versions support OP_COMMAND (in mongod but not mongos).
    return (version.minWireVersion <= WireVersion::FIND_COMMAND) &&
        (version.maxWireVersion >= WireVersion::FIND_COMMAND);
}

ProtocolSet computeProtocolSet(const WireVersionInfo version) {
    ProtocolSet result = supports::kNone;
    if (version.minWireVersion <= version.maxWireVersion) {
        if (version.maxWireVersion >= WireVersion::FIND_COMMAND) {
            result |= supports::kOpCommandOnly;
        }
        if (version.minWireVersion <= WireVersion::RELEASE_2_4_AND_BEFORE) {
            result |= supports::kOpQueryOnly;
        }
    }
    return result;
}

Status validateWireVersion(const WireVersionInfo client, const WireVersionInfo server) {
    // Since this is defined in the code, it should always hold true since this is the versions that
    // mongos/d wants to connect to.
    invariant(client.minWireVersion <= client.maxWireVersion);

    // Server may return bad data.
    if (server.minWireVersion > server.maxWireVersion) {
        return Status(ErrorCodes::IncompatibleServerVersion,
                      str::stream() << "Server min and max wire version are incorrect ("
                                    << server.minWireVersion
                                    << ","
                                    << server.maxWireVersion
                                    << ")");
    }

    // Determine if the [min, max] tuples overlap.
    // We assert the invariant that min < max above.
    if (!(client.minWireVersion <= server.maxWireVersion &&
          client.maxWireVersion >= server.minWireVersion)) {
        return Status(ErrorCodes::IncompatibleServerVersion,
                      str::stream() << "Server min and max wire version are incompatible ("
                                    << server.minWireVersion
                                    << ","
                                    << server.maxWireVersion
                                    << ") with client min wire version ("
                                    << client.minWireVersion
                                    << ","
                                    << client.maxWireVersion
                                    << ")");
    }

    return Status::OK();
}

}  // namespace rpc
}  // namespace mongo
