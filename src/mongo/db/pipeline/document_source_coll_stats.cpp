
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

#include "mongo/db/pipeline/document_source_coll_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/stats/top.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/time_support.h"

using boost::intrusive_ptr;

namespace mongo {

REGISTER_DOCUMENT_SOURCE(collStats,
                         DocumentSourceCollStats::LiteParsed::parse,
                         DocumentSourceCollStats::createFromBson);

const char* DocumentSourceCollStats::getSourceName() const {
    return "$collStats";
}

intrusive_ptr<DocumentSource> DocumentSourceCollStats::createFromBson(
    BSONElement specElem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40166,
            str::stream() << "$collStats must take a nested object but found: " << specElem,
            specElem.type() == BSONType::Object);
    intrusive_ptr<DocumentSourceCollStats> collStats(new DocumentSourceCollStats(pExpCtx));

    for (const auto& elem : specElem.embeddedObject()) {
        StringData fieldName = elem.fieldNameStringData();

        if ("latencyStats" == fieldName) {
            uassert(40167,
                    str::stream() << "latencyStats argument must be an object, but got " << elem
                                  << " of type "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Object);
            if (!elem["histograms"].eoo()) {
                uassert(40305,
                        str::stream() << "histograms option to latencyStats must be bool, got "
                                      << elem
                                      << "of type "
                                      << typeName(elem.type()),
                        elem["histograms"].isBoolean());
            }
        } else if ("storageStats" == fieldName) {
            uassert(40279,
                    str::stream() << "storageStats argument must be an object, but got " << elem
                                  << " of type "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Object);
        } else if ("count" == fieldName) {
            uassert(40480,
                    str::stream() << "count argument must be an object, but got " << elem
                                  << " of type "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Object);
        } else {
            uasserted(40168, str::stream() << "unrecognized option to $collStats: " << fieldName);
        }
    }

    collStats->_collStatsSpec = specElem.Obj().getOwned();
    return collStats;
}

DocumentSource::GetNextResult DocumentSourceCollStats::getNext() {
    pExpCtx->checkForInterrupt();

    if (_finished) {
        return GetNextResult::makeEOF();
    }

    _finished = true;

    BSONObjBuilder builder;

    builder.append("ns", pExpCtx->ns.ns());

    auto shardName = pExpCtx->mongoProcessInterface->getShardName(pExpCtx->opCtx);

    if (!shardName.empty()) {
        builder.append("shard", shardName);
    }

    builder.append("host", getHostNameCachedAndPort());
    builder.appendDate("localTime", jsTime());

    if (_collStatsSpec.hasField("latencyStats")) {
        // If the latencyStats field exists, it must have been validated as an object when parsing.
        bool includeHistograms = false;
        if (_collStatsSpec["latencyStats"].type() == BSONType::Object) {
            includeHistograms = _collStatsSpec["latencyStats"]["histograms"].boolean();
        }
        pExpCtx->mongoProcessInterface->appendLatencyStats(
            pExpCtx->opCtx, pExpCtx->ns, includeHistograms, &builder);
    }

    if (_collStatsSpec.hasField("storageStats")) {
        // If the storageStats field exists, it must have been validated as an object when parsing.
        BSONObjBuilder storageBuilder(builder.subobjStart("storageStats"));
        Status status = pExpCtx->mongoProcessInterface->appendStorageStats(
            pExpCtx->opCtx, pExpCtx->ns, _collStatsSpec["storageStats"].Obj(), &storageBuilder);
        storageBuilder.doneFast();
        if (!status.isOK()) {
            uasserted(40280,
                      str::stream() << "Unable to retrieve storageStats in $collStats stage: "
                                    << status.reason());
        }
    }

    if (_collStatsSpec.hasField("count")) {
        Status status = pExpCtx->mongoProcessInterface->appendRecordCount(
            pExpCtx->opCtx, pExpCtx->ns, &builder);
        if (!status.isOK()) {
            uasserted(40481,
                      str::stream() << "Unable to retrieve count in $collStats stage: "
                                    << status.reason());
        }
    }

    return {Document(builder.obj())};
}

Value DocumentSourceCollStats::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(Document{{getSourceName(), _collStatsSpec}});
}

}  // namespace mongo
