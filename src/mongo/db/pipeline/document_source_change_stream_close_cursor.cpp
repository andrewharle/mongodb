
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

#include "mongo/db/pipeline/document_source_change_stream_close_cursor.h"

namespace mongo {

namespace {

// Returns true if the given 'operationType' should invalidate the change stream based on the
// namespace in 'pExpCtx'.
bool isInvalidatingCommand(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                           StringData operationType) {
    if (pExpCtx->isSingleNamespaceAggregation()) {
        return operationType == DocumentSourceChangeStream::kDropCollectionOpType ||
            operationType == DocumentSourceChangeStream::kRenameCollectionOpType ||
            operationType == DocumentSourceChangeStream::kDropDatabaseOpType;
    } else if (!pExpCtx->isClusterAggregation()) {
        return operationType == DocumentSourceChangeStream::kDropDatabaseOpType;
    } else {
        return false;
    }
};

}  // namespace

DocumentSource::GetNextResult DocumentSourceCloseCursor::getNext() {
    pExpCtx->checkForInterrupt();

    // Close cursor if we have returned an invalidate entry.
    if (_shouldCloseCursor) {
        uasserted(ErrorCodes::CloseChangeStream, "Change stream has been invalidated");
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced())
        return nextInput;

    auto doc = nextInput.getDocument();
    const auto& kOperationTypeField = DocumentSourceChangeStream::kOperationTypeField;
    DocumentSourceChangeStream::checkValueType(
        doc[kOperationTypeField], kOperationTypeField, BSONType::String);
    auto operationType = doc[kOperationTypeField].getString();
    if (operationType == DocumentSourceChangeStream::kInvalidateOpType) {
        // Pass the invalidation forward, so that it can be included in the results, or
        // filtered/transformed by further stages in the pipeline, then throw an exception
        // to close the cursor on the next call to getNext().
        _shouldCloseCursor = true;
    }

    return nextInput;
}

}  // namespace mongo
