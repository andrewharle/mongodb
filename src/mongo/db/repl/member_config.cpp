
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

#include "mongo/db/repl/member_config.h"

#include <boost/algorithm/string.hpp>

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

const std::string MemberConfig::kIdFieldName = "_id";
const std::string MemberConfig::kVotesFieldName = "votes";
const std::string MemberConfig::kPriorityFieldName = "priority";
const std::string MemberConfig::kHostFieldName = "host";
const std::string MemberConfig::kHiddenFieldName = "hidden";
const std::string MemberConfig::kSlaveDelayFieldName = "slaveDelay";
const std::string MemberConfig::kArbiterOnlyFieldName = "arbiterOnly";
const std::string MemberConfig::kBuildIndexesFieldName = "buildIndexes";
const std::string MemberConfig::kTagsFieldName = "tags";
const std::string MemberConfig::kHorizonsFieldName = "horizons";
const std::string MemberConfig::kInternalVoterTagName = "$voter";
const std::string MemberConfig::kInternalElectableTagName = "$electable";
const std::string MemberConfig::kInternalAllTagName = "$all";

namespace {
const std::string kLegalMemberConfigFieldNames[] = {MemberConfig::kIdFieldName,
                                                    MemberConfig::kVotesFieldName,
                                                    MemberConfig::kPriorityFieldName,
                                                    MemberConfig::kHostFieldName,
                                                    MemberConfig::kHiddenFieldName,
                                                    MemberConfig::kSlaveDelayFieldName,
                                                    MemberConfig::kArbiterOnlyFieldName,
                                                    MemberConfig::kBuildIndexesFieldName,
                                                    MemberConfig::kTagsFieldName,
                                                    MemberConfig::kHorizonsFieldName};

const int kVotesFieldDefault = 1;
const double kPriorityFieldDefault = 1.0;
const Seconds kSlaveDelayFieldDefault(0);
const bool kArbiterOnlyFieldDefault = false;
const bool kHiddenFieldDefault = false;
const bool kBuildIndexesFieldDefault = true;

const Seconds kMaxSlaveDelay(3600 * 24 * 366);

}  // namespace

MemberConfig::MemberConfig(const BSONObj& mcfg, ReplSetTagConfig* tagConfig) {
    uassertStatusOK(bsonCheckOnlyHasFields(
        "replica set member configuration", mcfg, kLegalMemberConfigFieldNames));

    //
    // Parse _id field.
    //
    BSONElement idElement = mcfg[kIdFieldName];
    if (idElement.eoo())
        uasserted(ErrorCodes::NoSuchKey, str::stream() << kIdFieldName << " field is missing");

    if (!idElement.isNumber())
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << kIdFieldName << " field has non-numeric type "
                                << typeName(idElement.type()));
    _id = idElement.numberInt();

    //
    // Parse h field.
    //
    std::string hostAndPortString;
    uassertStatusOK(bsonExtractStringField(mcfg, kHostFieldName, &hostAndPortString));
    boost::trim(hostAndPortString);
    HostAndPort host;
    uassertStatusOK(host.initialize(hostAndPortString));
    if (!host.hasPort()) {
        // make port explicit even if default.
        host = HostAndPort(host.host(), host.port());
    }

    //
    // Parse votes field.
    //
    BSONElement votesElement = mcfg[kVotesFieldName];
    if (votesElement.eoo()) {
        _votes = kVotesFieldDefault;
    } else if (votesElement.isNumber()) {
        _votes = votesElement.numberInt();
    } else {
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << kVotesFieldName << " field value has non-numeric type "
                                << typeName(votesElement.type()));
    }

    //
    // Parse arbiterOnly field.
    //
    uassertStatusOK(bsonExtractBooleanFieldWithDefault(
        mcfg, kArbiterOnlyFieldName, kArbiterOnlyFieldDefault, &_arbiterOnly));

    //
    // Parse priority field.
    //
    BSONElement priorityElement = mcfg[kPriorityFieldName];
    if (priorityElement.eoo() ||
        (priorityElement.isNumber() && priorityElement.numberDouble() == kPriorityFieldDefault)) {
        _priority = _arbiterOnly ? 0.0 : kPriorityFieldDefault;
    } else if (priorityElement.isNumber()) {
        _priority = priorityElement.numberDouble();
    } else {
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << kPriorityFieldName << " field has non-numeric type "
                                << typeName(priorityElement.type()));
    }

    //
    // Parse slaveDelay field.
    //
    BSONElement slaveDelayElement = mcfg[kSlaveDelayFieldName];
    if (slaveDelayElement.eoo()) {
        _slaveDelay = kSlaveDelayFieldDefault;
    } else if (slaveDelayElement.isNumber()) {
        _slaveDelay = Seconds(slaveDelayElement.numberInt());
    } else {
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << kSlaveDelayFieldName << " field value has non-numeric type "
                                << typeName(slaveDelayElement.type()));
    }

    //
    // Parse hidden field.
    //
    uassertStatusOK(
        bsonExtractBooleanFieldWithDefault(mcfg, kHiddenFieldName, kHiddenFieldDefault, &_hidden));

    //
    // Parse buildIndexes field.
    //
    uassertStatusOK(bsonExtractBooleanFieldWithDefault(
        mcfg, kBuildIndexesFieldName, kBuildIndexesFieldDefault, &_buildIndexes));

    //
    // Parse "tags" field.
    //
    try {
        BSONElement tagsElement;
        uassertStatusOK(bsonExtractTypedField(mcfg, kTagsFieldName, Object, &tagsElement));
        for (auto&& tag : tagsElement.Obj()) {
            if (tag.type() != String) {
                uasserted(ErrorCodes::TypeMismatch,
                          str::stream() << "tags." << tag.fieldName()
                                        << " field has non-string value of type "
                                        << typeName(tag.type()));
            }
            _tags.push_back(tagConfig->makeTag(tag.fieldNameStringData(), tag.valueStringData()));
        }
    } catch (const ExceptionFor<ErrorCodes::NoSuchKey>&) {
        // No such key is okay in this case, everything else is a problem.
    }

    const auto horizonsElement = [&]() -> boost::optional<BSONElement> {
        const BSONElement result = mcfg[kHorizonsFieldName];
        if (result.eoo()) {
            return boost::none;
        }
        return result;
    }();

    _splitHorizon = SplitHorizon(host, horizonsElement);

    //
    // Add internal tags based on other member properties.
    //

    // Add a voter tag if this non-arbiter member votes; use _id for uniquity.
    const std::string id = str::stream() << _id;
    if (isVoter() && !_arbiterOnly) {
        _tags.push_back(tagConfig->makeTag(kInternalVoterTagName, id));
    }

    // Add an electable tag if this member is electable.
    if (isElectable()) {
        _tags.push_back(tagConfig->makeTag(kInternalElectableTagName, id));
    }

    // Add a tag for generic counting of this node.
    if (!_arbiterOnly) {
        _tags.push_back(tagConfig->makeTag(kInternalAllTagName, id));
    }
}

Status MemberConfig::validate() const {
    if (_id < 0 || _id > 255) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kIdFieldName << " field value of " << _id
                                    << " is out of range.");
    }

    if (_priority < 0 || _priority > 1000) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kPriorityFieldName << " field value of " << _priority
                                    << " is out of range");
    }
    if (_votes != 0 && _votes != 1) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kVotesFieldName << " field value is " << _votes
                                    << " but must be 0 or 1");
    }
    if (_arbiterOnly) {
        if (!_tags.empty()) {
            return Status(ErrorCodes::BadValue, "Cannot set tags on arbiters.");
        }
        if (!isVoter()) {
            return Status(ErrorCodes::BadValue, "Arbiter must vote (cannot have 0 votes)");
        }
    }
    if (_slaveDelay < Seconds(0) || _slaveDelay > kMaxSlaveDelay) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kSlaveDelayFieldName << " field value of "
                                    << durationCount<Seconds>(_slaveDelay)
                                    << " seconds is out of range");
    }
    // Check for additional electable requirements, when priority is non zero
    if (_priority != 0) {
        if (_votes == 0) {
            return Status(ErrorCodes::BadValue, "priority must be 0 when non-voting (votes:0)");
        }
        if (_slaveDelay > Seconds(0)) {
            return Status(ErrorCodes::BadValue, "priority must be 0 when slaveDelay is used");
        }
        if (_hidden) {
            return Status(ErrorCodes::BadValue, "priority must be 0 when hidden=true");
        }
        if (!_buildIndexes) {
            return Status(ErrorCodes::BadValue, "priority must be 0 when buildIndexes=false");
        }
    }
    return Status::OK();
}

bool MemberConfig::hasTags(const ReplSetTagConfig& tagConfig) const {
    for (std::vector<ReplSetTag>::const_iterator tag = _tags.begin(); tag != _tags.end(); tag++) {
        std::string tagKey = tagConfig.getTagKey(*tag);
        if (tagKey[0] == '$') {
            // Filter out internal tags
            continue;
        }
        return true;
    }
    return false;
}

BSONObj MemberConfig::toBSON(const ReplSetTagConfig& tagConfig) const {
    BSONObjBuilder configBuilder;
    configBuilder.append("_id", _id);
    configBuilder.append("host", _host().toString());
    configBuilder.append("arbiterOnly", _arbiterOnly);
    configBuilder.append("buildIndexes", _buildIndexes);
    configBuilder.append("hidden", _hidden);
    configBuilder.append("priority", _priority);

    BSONObjBuilder tags(configBuilder.subobjStart("tags"));
    for (std::vector<ReplSetTag>::const_iterator tag = _tags.begin(); tag != _tags.end(); tag++) {
        std::string tagKey = tagConfig.getTagKey(*tag);
        if (tagKey[0] == '$') {
            // Filter out internal tags
            continue;
        }
        tags.append(tagKey, tagConfig.getTagValue(*tag));
    }
    tags.done();

    _splitHorizon.toBSON(configBuilder);

    configBuilder.append("slaveDelay", durationCount<Seconds>(_slaveDelay));
    configBuilder.append("votes", getNumVotes());
    return configBuilder.obj();
}

}  // namespace repl
}  // namespace mongo
