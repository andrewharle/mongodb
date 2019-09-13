
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
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/s/stale_exception.h"

namespace mongo {

/**
 * The result of performing a single write, possibly within a batch.
 */
struct WriteResult {
    /**
     * Maps 1-to-1 to single ops in request. May be shorter than input if there are errors.
     */
    std::vector<StatusWith<SingleWriteResult>> results;
};


/**
 * Performs a batch of inserts, updates, or deletes.
 *
 * These functions handle all of the work of doing the writes, including locking, incrementing
 * counters, managing CurOp, and of course actually doing the write. Waiting for the writeConcern is
 * *not* handled by these functions and is expected to be done by the caller if needed.
 *
 * LastError is updated for failures of individual writes, but not for batch errors reported by an
 * exception being thrown from these functions. Callers are responsible for managing LastError in
 * that case. This should generally be combined with LastError handling from parse failures.
 *
 * 'fromMigrate' indicates whether the operation was induced by a chunk migration
 */
WriteResult performInserts(OperationContext* opCtx,
                           const write_ops::Insert& op,
                           bool fromMigrate = false);
WriteResult performUpdates(OperationContext* opCtx, const write_ops::Update& op);
WriteResult performDeletes(OperationContext* opCtx, const write_ops::Delete& op);

}  // namespace mongo
