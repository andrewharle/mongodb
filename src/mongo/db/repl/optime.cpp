
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

#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/optime.h"

namespace mongo {
namespace repl {

const char OpTime::kTimestampFieldName[] = "ts";
const char OpTime::kTermFieldName[] = "t";
const long long OpTime::kInitialTerm = 0;

// static
OpTime OpTime::max() {
    return OpTime(Timestamp::max(), std::numeric_limits<decltype(OpTime::_term)>::max());
}

void OpTime::append(BSONObjBuilder* builder, const std::string& subObjName) const {
    BSONObjBuilder opTimeBuilder(builder->subobjStart(subObjName));
    opTimeBuilder.append(kTimestampFieldName, _timestamp);

    opTimeBuilder.append(kTermFieldName, _term);
    opTimeBuilder.doneFast();
}

StatusWith<OpTime> OpTime::parseFromOplogEntry(const BSONObj& obj) {
    Timestamp ts;
    Status status = bsonExtractTimestampField(obj, kTimestampFieldName, &ts);
    if (!status.isOK())
        return status;

    // Default to -1 if the term is absent.
    long long term;
    status = bsonExtractIntegerFieldWithDefault(obj, kTermFieldName, kUninitializedTerm, &term);
    if (!status.isOK())
        return status;

    return OpTime(ts, term);
}

BSONObj OpTime::toBSON() const {
    BSONObjBuilder bldr;
    bldr.append(kTimestampFieldName, _timestamp);
    bldr.append(kTermFieldName, _term);
    return bldr.obj();
}

// static
OpTime OpTime::parse(const BSONObj& obj) {
    return uassertStatusOK(parseFromOplogEntry(obj));
}

std::string OpTime::toString() const {
    return toBSON().toString();
}

std::ostream& operator<<(std::ostream& out, const OpTime& opTime) {
    return out << opTime.toString();
}

void OpTime::appendAsQuery(BSONObjBuilder* builder) const {
    builder->append(kTimestampFieldName, _timestamp);
    if (_term == kUninitializedTerm) {
        // pv0 oplogs don't actually have the term field so don't query for {t: -1}.
        builder->append(kTermFieldName, BSON("$exists" << false));
    } else {
        builder->append(kTermFieldName, _term);
    }
}

BSONObj OpTime::asQuery() const {
    BSONObjBuilder builder;
    appendAsQuery(&builder);
    return builder.obj();
}

}  // namespace repl

BSONObjBuilder& operator<<(BSONObjBuilderValueStream& builder, const repl::OpTime& value) {
    return builder << value.toBSON();
}

}  // namespace mongo
