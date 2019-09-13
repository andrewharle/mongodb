
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

#include "mongo/db/query/find_and_modify_request.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/write_concern.h"

namespace mongo {

namespace {
const char kCmdName[] = "findAndModify";
const char kQueryField[] = "query";
const char kSortField[] = "sort";
const char kCollationField[] = "collation";
const char kArrayFiltersField[] = "arrayFilters";
const char kRemoveField[] = "remove";
const char kUpdateField[] = "update";
const char kNewField[] = "new";
const char kFieldProjectionField[] = "fields";
const char kUpsertField[] = "upsert";
const char kWriteConcernField[] = "writeConcern";

const std::vector<BSONObj> emptyArrayFilters{};
}  // unnamed namespace

FindAndModifyRequest::FindAndModifyRequest(NamespaceString fullNs, BSONObj query, BSONObj updateObj)
    : _ns(std::move(fullNs)),
      _query(query.getOwned()),
      _updateObj(updateObj.getOwned()),
      _isRemove(false) {}

FindAndModifyRequest FindAndModifyRequest::makeUpdate(NamespaceString fullNs,
                                                      BSONObj query,
                                                      BSONObj updateObj) {
    return FindAndModifyRequest(fullNs, query, updateObj);
}

FindAndModifyRequest FindAndModifyRequest::makeRemove(NamespaceString fullNs, BSONObj query) {
    FindAndModifyRequest request(fullNs, query, BSONObj());
    request._isRemove = true;
    return request;
}

BSONObj FindAndModifyRequest::toBSON() const {
    BSONObjBuilder builder;

    builder.append(kCmdName, _ns.coll());
    builder.append(kQueryField, _query);

    if (_isRemove) {
        builder.append(kRemoveField, true);
    } else {
        builder.append(kUpdateField, _updateObj);

        if (_isUpsert) {
            builder.append(kUpsertField, _isUpsert.get());
        }
    }

    if (_fieldProjection) {
        builder.append(kFieldProjectionField, _fieldProjection.get());
    }

    if (_sort) {
        builder.append(kSortField, _sort.get());
    }

    if (_collation) {
        builder.append(kCollationField, _collation.get());
    }

    if (_arrayFilters) {
        BSONArrayBuilder arrayBuilder(builder.subarrayStart(kArrayFiltersField));
        for (auto arrayFilter : _arrayFilters.get()) {
            arrayBuilder.append(arrayFilter);
        }
        arrayBuilder.doneFast();
    }

    if (_shouldReturnNew) {
        builder.append(kNewField, _shouldReturnNew.get());
    }

    if (_writeConcern) {
        builder.append(kWriteConcernField, _writeConcern->toBSON());
    }

    return builder.obj();
}

StatusWith<FindAndModifyRequest> FindAndModifyRequest::parseFromBSON(NamespaceString fullNs,
                                                                     const BSONObj& cmdObj) {
    BSONObj query;
    BSONObj sort;
    BSONObj fields;
    if (auto queryElement = cmdObj[kQueryField]) {
        if (queryElement.type() != Object) {
            return {ErrorCodes::Error(31160),
                    str::stream() << "'" << kQueryField << "' parameter must be an object, found "
                                  << queryElement.type()};
        }
        query = queryElement.embeddedObject();
    }

    if (auto sortElement = cmdObj[kSortField]) {
        if (sortElement.type() != Object) {
            return {ErrorCodes::Error(31174),
                    str::stream() << "'" << kSortField << "' parameter must be an object, found "
                                  << sortElement.type()};
        }
        sort = sortElement.embeddedObject();
    }

    if (auto projectionElement = cmdObj[kFieldProjectionField]) {
        if (projectionElement.type() != Object) {
            return {ErrorCodes::Error(31175),
                    str::stream() << "'" << kFieldProjectionField
                                  << "' parameter must be an object, found "
                                  << projectionElement.type()};
        }
        fields = projectionElement.embeddedObject();
    }

    BSONObj updateObj = cmdObj.getObjectField(kUpdateField);

    BSONObj collation;
    {
        BSONElement collationElt;
        Status collationEltStatus =
            bsonExtractTypedField(cmdObj, kCollationField, BSONType::Object, &collationElt);
        if (!collationEltStatus.isOK() && (collationEltStatus != ErrorCodes::NoSuchKey)) {
            return collationEltStatus;
        }
        if (collationEltStatus.isOK()) {
            collation = collationElt.Obj();
        }
    }

    std::vector<BSONObj> arrayFilters;
    bool arrayFiltersSet = false;
    {
        BSONElement arrayFiltersElt;
        Status arrayFiltersEltStatus =
            bsonExtractTypedField(cmdObj, kArrayFiltersField, BSONType::Array, &arrayFiltersElt);
        if (!arrayFiltersEltStatus.isOK() && (arrayFiltersEltStatus != ErrorCodes::NoSuchKey)) {
            return arrayFiltersEltStatus;
        }
        if (arrayFiltersEltStatus.isOK()) {
            arrayFiltersSet = true;
            for (auto arrayFilter : arrayFiltersElt.Obj()) {
                if (arrayFilter.type() != BSONType::Object) {
                    return {ErrorCodes::TypeMismatch,
                            str::stream() << "Each array filter must be an object, found "
                                          << arrayFilter.type()};
                }
                arrayFilters.push_back(arrayFilter.Obj());
            }
        }
    }

    bool shouldReturnNew = cmdObj[kNewField].trueValue();
    bool isUpsert = cmdObj[kUpsertField].trueValue();
    bool isRemove = cmdObj[kRemoveField].trueValue();
    bool isUpdate = cmdObj.hasField(kUpdateField);

    if (!isRemove && !isUpdate) {
        return {ErrorCodes::FailedToParse, "Either an update or remove=true must be specified"};
    }

    if (isRemove) {
        if (isUpdate) {
            return {ErrorCodes::FailedToParse, "Cannot specify both an update and remove=true"};
        }

        if (isUpsert) {
            return {ErrorCodes::FailedToParse, "Cannot specify both upsert=true and remove=true"};
        }

        if (shouldReturnNew) {
            return {ErrorCodes::FailedToParse,
                    "Cannot specify both new=true and remove=true;"
                    " 'remove' always returns the deleted document"};
        }

        if (arrayFiltersSet) {
            return {ErrorCodes::FailedToParse, "Cannot specify arrayFilters and remove=true"};
        }
    }

    FindAndModifyRequest request(std::move(fullNs), query, updateObj);
    request._isRemove = isRemove;
    request.setFieldProjection(fields);
    request.setSort(sort);
    request.setCollation(collation);
    request.setArrayFilters(std::move(arrayFilters));

    if (!isRemove) {
        request.setShouldReturnNew(shouldReturnNew);
        request.setUpsert(isUpsert);
    }

    return request;
}

void FindAndModifyRequest::setFieldProjection(BSONObj fields) {
    _fieldProjection = fields.getOwned();
}

void FindAndModifyRequest::setSort(BSONObj sort) {
    _sort = sort.getOwned();
}

void FindAndModifyRequest::setCollation(BSONObj collation) {
    _collation = collation.getOwned();
}

void FindAndModifyRequest::setArrayFilters(const std::vector<BSONObj>& arrayFilters) {
    _arrayFilters = std::vector<BSONObj>();
    for (auto arrayFilter : arrayFilters) {
        _arrayFilters->emplace_back(arrayFilter.getOwned());
    }
}

void FindAndModifyRequest::setShouldReturnNew(bool shouldReturnNew) {
    dassert(!_isRemove);
    _shouldReturnNew = shouldReturnNew;
}

void FindAndModifyRequest::setUpsert(bool upsert) {
    dassert(!_isRemove);
    _isUpsert = upsert;
}

void FindAndModifyRequest::setWriteConcern(WriteConcernOptions writeConcern) {
    _writeConcern = std::move(writeConcern);
}

const NamespaceString& FindAndModifyRequest::getNamespaceString() const {
    return _ns;
}

BSONObj FindAndModifyRequest::getQuery() const {
    return _query;
}

BSONObj FindAndModifyRequest::getFields() const {
    return _fieldProjection.value_or(BSONObj());
}

BSONObj FindAndModifyRequest::getUpdateObj() const {
    return _updateObj;
}

BSONObj FindAndModifyRequest::getSort() const {
    return _sort.value_or(BSONObj());
}

BSONObj FindAndModifyRequest::getCollation() const {
    return _collation.value_or(BSONObj());
}

const std::vector<BSONObj>& FindAndModifyRequest::getArrayFilters() const {
    if (_arrayFilters) {
        return _arrayFilters.get();
    }
    return emptyArrayFilters;
}

bool FindAndModifyRequest::shouldReturnNew() const {
    return _shouldReturnNew.value_or(false);
}

bool FindAndModifyRequest::isUpsert() const {
    return _isUpsert.value_or(false);
}

bool FindAndModifyRequest::isRemove() const {
    return _isRemove;
}
}
