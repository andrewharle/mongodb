
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

#include "mongo/db/catalog/collection_options.h"

#include <algorithm>

#include "mongo/base/string_data.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

// TODO(SERVER-34489) Remove when upgrade/downgrade is ready.
bool createTimestampSafeUniqueIndex = false;
ExportedServerParameter<bool, ServerParameterType::kStartupOnly>
    createTimestampSafeUniqueIndexParameter(ServerParameterSet::getGlobal(),
                                            "createTimestampSafeUniqueIndex",
                                            &createTimestampSafeUniqueIndex);

// static
bool CollectionOptions::validMaxCappedDocs(long long* max) {
    if (*max <= 0 || *max == std::numeric_limits<long long>::max()) {
        *max = 0x7fffffff;
        return true;
    }

    if (*max < (0x1LL << 31)) {
        return true;
    }

    return false;
}

namespace {

Status checkStorageEngineOptions(const BSONElement& elem) {
    invariant(elem.fieldNameStringData() == "storageEngine");

    // Storage engine-specific collection options.
    // "storageEngine" field must be of type "document".
    // Every field inside "storageEngine" has to be a document.
    // Format:
    // {
    //     ...
    //     storageEngine: {
    //         storageEngine1: {
    //             ...
    //         },
    //         storageEngine2: {
    //             ...
    //         }
    //     },
    //     ...
    // }
    if (elem.type() != mongo::Object) {
        return {ErrorCodes::BadValue, "'storageEngine' has to be a document."};
    }

    BSONForEach(storageEngineElement, elem.Obj()) {
        StringData storageEngineName = storageEngineElement.fieldNameStringData();
        if (storageEngineElement.type() != mongo::Object) {
            return {ErrorCodes::BadValue,
                    str::stream() << "'storageEngine." << storageEngineName
                                  << "' has to be an embedded document."};
        }
    }

    return Status::OK();
}

}  // namespace

bool CollectionOptions::isView() const {
    return !viewOn.empty();
}

Status CollectionOptions::validateForStorage() const {
    return CollectionOptions().parse(toBSON(), ParseKind::parseForStorage);
}

Status CollectionOptions::parse(const BSONObj& options, ParseKind kind) {
    *this = {};

    // Versions 2.4 and earlier of the server store "create" inside the collection metadata when the
    // user issues an explicit collection creation command. These versions also wrote any
    // unrecognized fields into the catalog metadata and allowed the order of these fields to be
    // changed. Therefore, if the "create" field is present, we must ignore any
    // unknown fields during parsing. Otherwise, we disallow unknown collection options.
    //
    // Versions 2.6 through 3.2 ignored unknown collection options rather than failing but did not
    // store the "create" field. These versions also refrained from materializing the unknown
    // options in the catalog, so we are free to fail on unknown options in this case.
    const bool createdOn24OrEarlier = static_cast<bool>(options["create"]);

    // During parsing, ignore some validation errors in order to accept options objects that
    // were valid in previous versions of the server.  SERVER-13737.
    BSONObjIterator i(options);

    while (i.more()) {
        BSONElement e = i.next();
        StringData fieldName = e.fieldName();

        if (fieldName == "uuid" && kind == parseForStorage) {
            auto res = CollectionUUID::parse(e);
            if (!res.isOK()) {
                return res.getStatus();
            }
            uuid = res.getValue();
        } else if (fieldName == "capped") {
            capped = e.trueValue();
        } else if (fieldName == "size") {
            if (!e.isNumber()) {
                // Ignoring for backwards compatibility.
                continue;
            }
            cappedSize = e.numberLong();
            if (cappedSize < 0)
                return Status(ErrorCodes::BadValue, "size has to be >= 0");
            const long long kGB = 1024 * 1024 * 1024;
            const long long kPB = 1024 * 1024 * kGB;
            if (cappedSize > kPB)
                return Status(ErrorCodes::BadValue, "size cannot exceed 1 PB");
            cappedSize += 0xff;
            cappedSize &= 0xffffffffffffff00LL;
        } else if (fieldName == "max") {
            if (!options["capped"].trueValue() || !e.isNumber()) {
                // Ignoring for backwards compatibility.
                continue;
            }
            cappedMaxDocs = e.numberLong();
            if (!validMaxCappedDocs(&cappedMaxDocs))
                return Status(ErrorCodes::BadValue,
                              "max in a capped collection has to be < 2^31 or not set");
        } else if (fieldName == "$nExtents") {
            if (e.type() == Array) {
                BSONObjIterator j(e.Obj());
                while (j.more()) {
                    BSONElement inner = j.next();
                    initialExtentSizes.push_back(inner.numberInt());
                }
            } else {
                initialNumExtents = e.numberLong();
            }
        } else if (fieldName == "autoIndexId") {
            if (e.trueValue())
                autoIndexId = YES;
            else
                autoIndexId = NO;
        } else if (fieldName == "flags") {
            flags = e.numberInt();
            flagsSet = true;
        } else if (fieldName == "temp") {
            temp = e.trueValue();
        } else if (fieldName == "storageEngine") {
            Status status = checkStorageEngineOptions(e);
            if (!status.isOK()) {
                return status;
            }
            storageEngine = e.Obj().getOwned();
        } else if (fieldName == "indexOptionDefaults") {
            if (e.type() != mongo::Object) {
                return {ErrorCodes::TypeMismatch, "'indexOptionDefaults' has to be a document."};
            }
            BSONForEach(option, e.Obj()) {
                if (option.fieldNameStringData() == "storageEngine") {
                    Status status = checkStorageEngineOptions(option);
                    if (!status.isOK()) {
                        return status.withContext("Error in indexOptionDefaults");
                    }
                } else {
                    // Return an error on first unrecognized field.
                    return {ErrorCodes::InvalidOptions,
                            str::stream() << "indexOptionDefaults." << option.fieldNameStringData()
                                          << " is not a supported option."};
                }
            }
            indexOptionDefaults = e.Obj().getOwned();
        } else if (fieldName == "validator") {
            if (e.type() != mongo::Object) {
                return Status(ErrorCodes::BadValue, "'validator' has to be a document.");
            }

            validator = e.Obj().getOwned();
        } else if (fieldName == "validationAction") {
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::BadValue, "'validationAction' has to be a string.");
            }

            validationAction = e.String();
        } else if (fieldName == "validationLevel") {
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::BadValue, "'validationLevel' has to be a string.");
            }

            validationLevel = e.String();
        } else if (fieldName == "collation") {
            if (e.type() != mongo::Object) {
                return Status(ErrorCodes::BadValue, "'collation' has to be a document.");
            }

            if (e.Obj().isEmpty()) {
                return Status(ErrorCodes::BadValue, "'collation' cannot be an empty document.");
            }

            collation = e.Obj().getOwned();
        } else if (fieldName == "viewOn") {
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::BadValue, "'viewOn' has to be a string.");
            }

            viewOn = e.String();
            if (viewOn.empty()) {
                return Status(ErrorCodes::BadValue, "'viewOn' cannot be empty.'");
            }
        } else if (fieldName == "pipeline") {
            if (e.type() != mongo::Array) {
                return Status(ErrorCodes::BadValue, "'pipeline' has to be an array.");
            }

            pipeline = e.Obj().getOwned();
        } else if (fieldName == "idIndex" && kind == parseForCommand) {
            if (e.type() != mongo::Object) {
                return Status(ErrorCodes::TypeMismatch, "'idIndex' has to be an object.");
            }

            auto tempIdIndex = e.Obj().getOwned();
            if (tempIdIndex.isEmpty()) {
                return {ErrorCodes::FailedToParse, "idIndex cannot be empty"};
            }

            idIndex = std::move(tempIdIndex);
        } else if (!createdOn24OrEarlier && !mongo::isGenericArgument(fieldName)) {
            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << "The field '" << fieldName
                                        << "' is not a valid collection option. Options: "
                                        << options);
        }
    }

    if (viewOn.empty() && !pipeline.isEmpty()) {
        return Status(ErrorCodes::BadValue, "'pipeline' cannot be specified without 'viewOn'");
    }

    return Status::OK();
}

BSONObj CollectionOptions::toBSON() const {
    BSONObjBuilder b;
    appendBSON(&b);
    return b.obj();
}

void CollectionOptions::appendBSON(BSONObjBuilder* builder) const {
    if (uuid) {
        builder->appendElements(uuid->toBSON());
    }

    if (capped) {
        builder->appendBool("capped", true);
        builder->appendNumber("size", cappedSize);

        if (cappedMaxDocs)
            builder->appendNumber("max", cappedMaxDocs);
    }

    if (initialNumExtents)
        builder->appendNumber("$nExtents", initialNumExtents);
    if (!initialExtentSizes.empty())
        builder->append("$nExtents", initialExtentSizes);

    if (autoIndexId != DEFAULT)
        builder->appendBool("autoIndexId", autoIndexId == YES);

    if (flagsSet)
        builder->append("flags", flags);

    if (temp)
        builder->appendBool("temp", true);

    if (!storageEngine.isEmpty()) {
        builder->append("storageEngine", storageEngine);
    }

    if (!indexOptionDefaults.isEmpty()) {
        builder->append("indexOptionDefaults", indexOptionDefaults);
    }

    if (!validator.isEmpty()) {
        builder->append("validator", validator);
    }

    if (!validationLevel.empty()) {
        builder->append("validationLevel", validationLevel);
    }

    if (!validationAction.empty()) {
        builder->append("validationAction", validationAction);
    }

    if (!collation.isEmpty()) {
        builder->append("collation", collation);
    }

    if (!viewOn.empty()) {
        builder->append("viewOn", viewOn);
    }

    if (!pipeline.isEmpty()) {
        builder->appendArray("pipeline", pipeline);
    }

    if (!idIndex.isEmpty()) {
        builder->append("idIndex", idIndex);
    }
}

bool CollectionOptions::matchesStorageOptions(const CollectionOptions& other,
                                              CollatorFactoryInterface* collatorFactory) const {
    if (capped != other.capped) {
        return false;
    }

    if (cappedSize != other.cappedSize) {
        return false;
    }

    if (cappedMaxDocs != other.cappedMaxDocs) {
        return false;
    }

    if (initialNumExtents != other.initialNumExtents) {
        return false;
    }

    if (initialExtentSizes.size() != other.initialExtentSizes.size()) {
        return false;
    }

    if (!std::equal(other.initialExtentSizes.begin(),
                    other.initialExtentSizes.end(),
                    initialExtentSizes.begin())) {
        return false;
    }

    if (autoIndexId != other.autoIndexId) {
        return false;
    }

    if (flagsSet != other.flagsSet) {
        return false;
    }

    if (flags != other.flags) {
        return false;
    }

    if (temp != other.temp) {
        return false;
    }

    if (storageEngine.woCompare(other.storageEngine) != 0) {
        return false;
    }

    if (indexOptionDefaults.woCompare(other.indexOptionDefaults) != 0) {
        return false;
    }

    if (validator.woCompare(other.validator) != 0) {
        return false;
    }

    if (validationAction != other.validationAction) {
        return false;
    }

    if (validationLevel != other.validationLevel) {
        return false;
    }

    // Note: the server can add more stuff on the collation options that were not specified in
    // the original user request. Use the collator to check for equivalence.
    auto myCollator =
        collation.isEmpty() ? nullptr : uassertStatusOK(collatorFactory->makeFromBSON(collation));
    auto otherCollator = other.collation.isEmpty()
        ? nullptr
        : uassertStatusOK(collatorFactory->makeFromBSON(other.collation));

    if (!CollatorInterface::collatorsMatch(myCollator.get(), otherCollator.get())) {
        return false;
    }

    if (viewOn != other.viewOn) {
        return false;
    }

    if (pipeline.woCompare(other.pipeline) != 0) {
        return false;
    }

    return true;
}
}
