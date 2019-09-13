
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

#include "mongo/db/pipeline/document_source_index_stats.h"

#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/server_options.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(indexStats,
                         DocumentSourceIndexStats::LiteParsed::parse,
                         DocumentSourceIndexStats::createFromBson);

const char* DocumentSourceIndexStats::getSourceName() const {
    return "$indexStats";
}

DocumentSource::GetNextResult DocumentSourceIndexStats::getNext() {
    pExpCtx->checkForInterrupt();

    if (_indexStatsMap.empty()) {
        _indexStatsMap = pExpCtx->mongoProcessInterface->getIndexStats(pExpCtx->opCtx, pExpCtx->ns);
        _indexStatsIter = _indexStatsMap.begin();
    }

    if (_indexStatsIter != _indexStatsMap.end()) {
        const auto& stats = _indexStatsIter->second;
        MutableDocument doc;
        doc["name"] = Value(_indexStatsIter->first);
        doc["key"] = Value(stats.indexKey);
        doc["host"] = Value(_processName);
        doc["accesses"]["ops"] = Value(stats.accesses.loadRelaxed());
        doc["accesses"]["since"] = Value(stats.trackerStartTime);
        ++_indexStatsIter;
        return doc.freeze();
    }

    return GetNextResult::makeEOF();
}

DocumentSourceIndexStats::DocumentSourceIndexStats(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx), _processName(getHostNameCachedAndPort()) {}

intrusive_ptr<DocumentSource> DocumentSourceIndexStats::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(28803,
            "The $indexStats stage specification must be an empty object",
            elem.type() == Object && elem.Obj().isEmpty());
    return new DocumentSourceIndexStats(pExpCtx);
}

Value DocumentSourceIndexStats::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(DOC(getSourceName() << Document()));
}
}
