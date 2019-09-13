
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

#include "mongo/s/write_ops/batched_command_response.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;
using std::string;

using mongoutils::str::stream;

const BSONField<long long> BatchedCommandResponse::n("n", 0);
const BSONField<long long> BatchedCommandResponse::nModified("nModified", 0);
const BSONField<std::vector<BatchedUpsertDetail*>> BatchedCommandResponse::upsertDetails(
    "upserted");
const BSONField<OID> BatchedCommandResponse::electionId("electionId");
const BSONField<std::vector<WriteErrorDetail*>> BatchedCommandResponse::writeErrors("writeErrors");
const BSONField<WriteConcernErrorDetail*> BatchedCommandResponse::writeConcernError(
    "writeConcernError");

BatchedCommandResponse::BatchedCommandResponse() {
    clear();
}

BatchedCommandResponse::~BatchedCommandResponse() {
    unsetErrDetails();
    unsetUpsertDetails();
}

bool BatchedCommandResponse::isValid(std::string* errMsg) const {
    std::string dummy;
    if (errMsg == NULL) {
        errMsg = &dummy;
    }

    // All the mandatory fields must be present.
    if (!_isStatusSet) {
        *errMsg = stream() << "missing status fields";
        return false;
    }

    return true;
}

BSONObj BatchedCommandResponse::toBSON() const {
    BSONObjBuilder builder;

    invariant(_isStatusSet);
    uassertStatusOK(_status);

    if (_isNModifiedSet)
        builder.appendNumber(nModified(), _nModified);
    if (_isNSet)
        builder.appendNumber(n(), _n);

    if (_upsertDetails.get()) {
        BSONArrayBuilder upsertedBuilder(builder.subarrayStart(upsertDetails()));
        for (std::vector<BatchedUpsertDetail*>::const_iterator it = _upsertDetails->begin();
             it != _upsertDetails->end();
             ++it) {
            BSONObj upsertedDetailsDocument = (*it)->toBSON();
            upsertedBuilder.append(upsertedDetailsDocument);
        }
        upsertedBuilder.done();
    }

    if (_isLastOpSet) {
        if (_lastOp.getTerm() != repl::OpTime::kUninitializedTerm) {
            _lastOp.append(&builder, "opTime");
        } else {
            builder.append("opTime", _lastOp.getTimestamp());
        }
    }
    if (_isElectionIdSet)
        builder.appendOID(electionId(), const_cast<OID*>(&_electionId));

    if (_writeErrorDetails.get()) {
        auto errorMessage =
            [ errorCount = size_t(0), errorSize = size_t(0) ](StringData rawMessage) mutable {
            // Start truncating error messages once both of these limits are exceeded.
            constexpr size_t kErrorSizeTruncationMin = 1024 * 1024;
            constexpr size_t kErrorCountTruncationMin = 2;
            if (errorSize >= kErrorSizeTruncationMin && errorCount >= kErrorCountTruncationMin) {
                return ""_sd;
            }

            errorCount++;
            errorSize += rawMessage.size();
            return rawMessage;
        };

        BSONArrayBuilder errDetailsBuilder(builder.subarrayStart(writeErrors()));
        for (auto&& writeError : *_writeErrorDetails) {
            BSONObjBuilder errDetailsDocument(errDetailsBuilder.subobjStart());

            if (writeError->isIndexSet())
                builder.append(WriteErrorDetail::index(), writeError->getIndex());

            auto status = writeError->toStatus();
            builder.append(WriteErrorDetail::errCode(), status.code());
            builder.append(WriteErrorDetail::errCodeName(), status.codeString());
            builder.append(WriteErrorDetail::errMessage(), errorMessage(status.reason()));
            if (auto extra = _status.extraInfo())
                extra->serialize(&builder);  // TODO consider extra info size for truncation.

            if (writeError->isErrInfoSet())
                builder.append(WriteErrorDetail::errInfo(), writeError->getErrInfo());
        }
        errDetailsBuilder.done();
    }

    if (_wcErrDetails.get()) {
        builder.append(writeConcernError(), _wcErrDetails->toBSON());
    }

    return builder.obj();
}

bool BatchedCommandResponse::parseBSON(const BSONObj& source, string* errMsg) {
    clear();

    std::string dummy;
    if (!errMsg)
        errMsg = &dummy;

    _status = getStatusFromCommandResult(source);
    _isStatusSet = true;

    // We're using appendNumber on generation so we'll try a smaller type
    // (int) first and then fall back to the original type (long long).
    BSONField<int> fieldN(n());
    int tempN;
    auto fieldState = FieldParser::extract(source, fieldN, &tempN, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID) {
        // try falling back to a larger type
        fieldState = FieldParser::extract(source, n, &_n, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID)
            return false;
        _isNSet = fieldState == FieldParser::FIELD_SET;
    } else if (fieldState == FieldParser::FIELD_SET) {
        _isNSet = true;
        _n = tempN;
    }

    // We're using appendNumber on generation so we'll try a smaller type
    // (int) first and then fall back to the original type (long long).
    BSONField<int> fieldNModified(nModified());
    int intNModified;
    fieldState = FieldParser::extract(source, fieldNModified, &intNModified, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID) {
        // try falling back to a larger type
        fieldState = FieldParser::extract(source, nModified, &_nModified, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID)
            return false;
        _isNModifiedSet = fieldState == FieldParser::FIELD_SET;
    } else if (fieldState == FieldParser::FIELD_SET) {
        _isNModifiedSet = true;
        _nModified = intNModified;
    }

    std::vector<BatchedUpsertDetail*>* tempUpsertDetails = NULL;
    fieldState = FieldParser::extract(source, upsertDetails, &tempUpsertDetails, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _upsertDetails.reset(tempUpsertDetails);

    const BSONElement opTimeElement = source["opTime"];
    _isLastOpSet = true;
    if (opTimeElement.eoo()) {
        _isLastOpSet = false;
    } else if (opTimeElement.type() == bsonTimestamp) {
        _lastOp = repl::OpTime(opTimeElement.timestamp(), repl::OpTime::kUninitializedTerm);
    } else if (opTimeElement.type() == Date) {
        _lastOp = repl::OpTime(Timestamp(opTimeElement.date()), repl::OpTime::kUninitializedTerm);
    } else if (opTimeElement.type() == Object) {
        Status status = bsonExtractOpTimeField(source, "opTime", &_lastOp);
        if (!status.isOK()) {
            return false;
        }
    } else {
        return false;
    }

    fieldState = FieldParser::extract(source, electionId, &_electionId, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isElectionIdSet = fieldState == FieldParser::FIELD_SET;

    std::vector<WriteErrorDetail*>* tempErrDetails = NULL;
    fieldState = FieldParser::extract(source, writeErrors, &tempErrDetails, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _writeErrorDetails.reset(tempErrDetails);

    WriteConcernErrorDetail* wcError = NULL;
    fieldState = FieldParser::extract(source, writeConcernError, &wcError, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _wcErrDetails.reset(wcError);

    return true;
}

void BatchedCommandResponse::clear() {
    _status = Status::OK();
    _isStatusSet = false;

    _nModified = 0;
    _isNModifiedSet = false;

    _n = 0;
    _isNSet = false;

    _singleUpserted = BSONObj();
    _isSingleUpsertedSet = false;

    if (_upsertDetails.get()) {
        for (std::vector<BatchedUpsertDetail*>::const_iterator it = _upsertDetails->begin();
             it != _upsertDetails->end();
             ++it) {
            delete *it;
        };
        _upsertDetails.reset();
    }

    _lastOp = repl::OpTime();
    _isLastOpSet = false;

    _electionId = OID();
    _isElectionIdSet = false;

    if (_writeErrorDetails.get()) {
        for (std::vector<WriteErrorDetail*>::const_iterator it = _writeErrorDetails->begin();
             it != _writeErrorDetails->end();
             ++it) {
            delete *it;
        };
        _writeErrorDetails.reset();
    }

    _wcErrDetails.reset();
}

std::string BatchedCommandResponse::toString() const {
    return toBSON().toString();
}

void BatchedCommandResponse::setStatus(Status status) {
    _status = std::move(status);
    _isStatusSet = true;
}

void BatchedCommandResponse::setNModified(long long n) {
    _nModified = n;
    _isNModifiedSet = true;
}

void BatchedCommandResponse::unsetNModified() {
    _isNModifiedSet = false;
}

bool BatchedCommandResponse::isNModified() const {
    return _isNModifiedSet;
}

long long BatchedCommandResponse::getNModified() const {
    if (_isNModifiedSet) {
        return _nModified;
    } else {
        return nModified.getDefault();
    }
}

void BatchedCommandResponse::setN(long long n) {
    _n = n;
    _isNSet = true;
}

void BatchedCommandResponse::unsetN() {
    _isNSet = false;
}

bool BatchedCommandResponse::isNSet() const {
    return _isNSet;
}

long long BatchedCommandResponse::getN() const {
    if (_isNSet) {
        return _n;
    } else {
        return n.getDefault();
    }
}

void BatchedCommandResponse::setUpsertDetails(
    const std::vector<BatchedUpsertDetail*>& upsertDetails) {
    unsetUpsertDetails();
    for (std::vector<BatchedUpsertDetail*>::const_iterator it = upsertDetails.begin();
         it != upsertDetails.end();
         ++it) {
        unique_ptr<BatchedUpsertDetail> tempBatchedUpsertDetail(new BatchedUpsertDetail);
        (*it)->cloneTo(tempBatchedUpsertDetail.get());
        addToUpsertDetails(tempBatchedUpsertDetail.release());
    }
}

void BatchedCommandResponse::addToUpsertDetails(BatchedUpsertDetail* upsertDetails) {
    if (_upsertDetails.get() == NULL) {
        _upsertDetails.reset(new std::vector<BatchedUpsertDetail*>);
    }
    _upsertDetails->push_back(upsertDetails);
}

void BatchedCommandResponse::unsetUpsertDetails() {
    if (_upsertDetails.get() != NULL) {
        for (std::vector<BatchedUpsertDetail*>::iterator it = _upsertDetails->begin();
             it != _upsertDetails->end();
             ++it) {
            delete *it;
        }
        _upsertDetails.reset();
    }
}

bool BatchedCommandResponse::isUpsertDetailsSet() const {
    return _upsertDetails.get() != NULL;
}

size_t BatchedCommandResponse::sizeUpsertDetails() const {
    dassert(_upsertDetails.get());
    return _upsertDetails->size();
}

const std::vector<BatchedUpsertDetail*>& BatchedCommandResponse::getUpsertDetails() const {
    dassert(_upsertDetails.get());
    return *_upsertDetails;
}

const BatchedUpsertDetail* BatchedCommandResponse::getUpsertDetailsAt(size_t pos) const {
    dassert(_upsertDetails.get());
    dassert(_upsertDetails->size() > pos);
    return _upsertDetails->at(pos);
}

void BatchedCommandResponse::setLastOp(repl::OpTime lastOp) {
    _lastOp = lastOp;
    _isLastOpSet = true;
}

void BatchedCommandResponse::unsetLastOp() {
    _isLastOpSet = false;
}

bool BatchedCommandResponse::isLastOpSet() const {
    return _isLastOpSet;
}

repl::OpTime BatchedCommandResponse::getLastOp() const {
    dassert(_isLastOpSet);
    return _lastOp;
}

void BatchedCommandResponse::setElectionId(const OID& electionId) {
    _electionId = electionId;
    _isElectionIdSet = true;
}

void BatchedCommandResponse::unsetElectionId() {
    _isElectionIdSet = false;
}

bool BatchedCommandResponse::isElectionIdSet() const {
    return _isElectionIdSet;
}

OID BatchedCommandResponse::getElectionId() const {
    dassert(_isElectionIdSet);
    return _electionId;
}

void BatchedCommandResponse::setErrDetails(const std::vector<WriteErrorDetail*>& errDetails) {
    unsetErrDetails();
    for (std::vector<WriteErrorDetail*>::const_iterator it = errDetails.begin();
         it != errDetails.end();
         ++it) {
        unique_ptr<WriteErrorDetail> tempBatchErrorDetail(new WriteErrorDetail);
        (*it)->cloneTo(tempBatchErrorDetail.get());
        addToErrDetails(tempBatchErrorDetail.release());
    }
}

void BatchedCommandResponse::addToErrDetails(WriteErrorDetail* errDetails) {
    if (_writeErrorDetails.get() == NULL) {
        _writeErrorDetails.reset(new std::vector<WriteErrorDetail*>);
    }
    _writeErrorDetails->push_back(errDetails);
}

void BatchedCommandResponse::unsetErrDetails() {
    if (_writeErrorDetails.get() != NULL) {
        for (std::vector<WriteErrorDetail*>::iterator it = _writeErrorDetails->begin();
             it != _writeErrorDetails->end();
             ++it) {
            delete *it;
        }
        _writeErrorDetails.reset();
    }
}

bool BatchedCommandResponse::isErrDetailsSet() const {
    return _writeErrorDetails.get() != NULL;
}

size_t BatchedCommandResponse::sizeErrDetails() const {
    dassert(_writeErrorDetails.get());
    return _writeErrorDetails->size();
}

const std::vector<WriteErrorDetail*>& BatchedCommandResponse::getErrDetails() const {
    dassert(_writeErrorDetails.get());
    return *_writeErrorDetails;
}

const WriteErrorDetail* BatchedCommandResponse::getErrDetailsAt(size_t pos) const {
    dassert(_writeErrorDetails.get());
    dassert(_writeErrorDetails->size() > pos);
    return _writeErrorDetails->at(pos);
}

void BatchedCommandResponse::setWriteConcernError(WriteConcernErrorDetail* error) {
    _wcErrDetails.reset(error);
}

void BatchedCommandResponse::unsetWriteConcernError() {
    _wcErrDetails.reset();
}

bool BatchedCommandResponse::isWriteConcernErrorSet() const {
    return _wcErrDetails.get();
}

const WriteConcernErrorDetail* BatchedCommandResponse::getWriteConcernError() const {
    return _wcErrDetails.get();
}

Status BatchedCommandResponse::toStatus() const {
    if (!getOk()) {
        return _status;
    }

    if (isErrDetailsSet()) {
        return getErrDetails().front()->toStatus();
    }

    if (isWriteConcernErrorSet()) {
        return getWriteConcernError()->toStatus();
    }

    return Status::OK();
}

}  // namespace mongo
