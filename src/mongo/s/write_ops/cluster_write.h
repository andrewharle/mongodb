
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

#include <string>

#include "mongo/s/write_ops/batch_write_exec.h"

namespace mongo {

class BSONObj;
class Chunk;
class ChunkManager;
class OperationContext;

class ClusterWriter {
public:
    static void write(OperationContext* opCtx,
                      const BatchedCommandRequest& request,
                      BatchWriteExecStats* stats,
                      BatchedCommandResponse* response);
};

/**
 * Adds the specified amount of data written to the chunk's stats and if the total amount nears the
 * max size of a shard attempt to split the chunk. This call is opportunistic and swallows any
 * errors.
 */
void updateChunkWriteStatsAndSplitIfNeeded(OperationContext* opCtx,
                                           ChunkManager* manager,
                                           Chunk chunk,
                                           long dataWritten);

}  // namespace mongo
