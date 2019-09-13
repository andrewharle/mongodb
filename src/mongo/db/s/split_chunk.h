
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

#pragma once

#include <boost/optional.hpp>
#include <string>
#include <vector>

namespace mongo {

class BSONObj;
class ChunkRange;
class KeyPattern;
class NamespaceString;
class OID;
class OperationContext;
template <typename T>
class StatusWith;

/**
 * Attempts to split a chunk with the specified parameters. If the split fails, then the StatusWith
 * object returned will contain a Status with an ErrorCode regarding the cause of failure. If the
 * split succeeds, then the StatusWith object returned will contain Status::Ok().
 *
 * Additionally, splitChunk will attempt to perform top-chunk optimization. If top-chunk
 * optimization is performed, then the function will also return a ChunkRange, which contains the
 * range for the top chunk. Note that this ChunkRange is boost::optional, meaning that if top-chunk
 * optimization is not performed, boost::none will be returned inside of the StatusWith instead.
 */
StatusWith<boost::optional<ChunkRange>> splitChunk(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const BSONObj& keyPatternObj,
                                                   const ChunkRange& chunkRange,
                                                   const std::vector<BSONObj>& splitKeys,
                                                   const std::string& shardName,
                                                   const OID& expectedCollectionEpoch);

}  // namespace mongo
