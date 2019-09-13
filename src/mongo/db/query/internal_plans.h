
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

#include "mongo/base/string_data.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/record_id.h"

namespace mongo {

class BSONObj;
class Collection;
class IndexDescriptor;
class OperationContext;
class PlanStage;
class WorkingSet;
struct DeleteStageParams;
struct UpdateStageParams;

/**
 * The internal planner is a one-stop shop for "off-the-shelf" plans.  Most internal procedures
 * that do not require advanced queries could be served by plans already in here.
 */
class InternalPlanner {
public:
    enum Direction {
        FORWARD = 1,
        BACKWARD = -1,
    };

    enum IndexScanOptions {
        // The client is interested in the default outputs of an index scan: BSONObj of the key,
        // RecordId of the record that's indexed.  The client does its own fetching if required.
        IXSCAN_DEFAULT = 0,

        // The client wants the fetched object and the RecordId that refers to it.  Delegating
        // the fetch to the runner allows fetching outside of a lock.
        IXSCAN_FETCH = 1,
    };

    /**
     * Returns a collection scan.  Caller owns pointer.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> collectionScan(
        OperationContext* opCtx,
        StringData ns,
        Collection* collection,
        PlanExecutor::YieldPolicy yieldPolicy,
        const Direction direction = FORWARD,
        const RecordId startLoc = RecordId());

    /**
     * Returns a FETCH => DELETE plan.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> deleteWithCollectionScan(
        OperationContext* opCtx,
        Collection* collection,
        const DeleteStageParams& params,
        PlanExecutor::YieldPolicy yieldPolicy,
        Direction direction = FORWARD,
        const RecordId& startLoc = RecordId());

    /**
     * Returns an index scan.  Caller owns returned pointer.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> indexScan(
        OperationContext* opCtx,
        const Collection* collection,
        const IndexDescriptor* descriptor,
        const BSONObj& startKey,
        const BSONObj& endKey,
        BoundInclusion boundInclusion,
        PlanExecutor::YieldPolicy yieldPolicy,
        Direction direction = FORWARD,
        int options = IXSCAN_DEFAULT);

    /**
     * Returns an IXSCAN => FETCH => DELETE plan.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> deleteWithIndexScan(
        OperationContext* opCtx,
        Collection* collection,
        const DeleteStageParams& params,
        const IndexDescriptor* descriptor,
        const BSONObj& startKey,
        const BSONObj& endKey,
        BoundInclusion boundInclusion,
        PlanExecutor::YieldPolicy yieldPolicy,
        Direction direction = FORWARD);

    /**
     * Returns an IDHACK => UPDATE plan.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> updateWithIdHack(
        OperationContext* opCtx,
        Collection* collection,
        const UpdateStageParams& params,
        const IndexDescriptor* descriptor,
        const BSONObj& key,
        PlanExecutor::YieldPolicy yieldPolicy);

private:
    /**
     * Returns a plan stage that can be used for a collection scan.
     *
     * Used as a helper for collectionScan() and deleteWithCollectionScan().
     */
    static std::unique_ptr<PlanStage> _collectionScan(OperationContext* opCtx,
                                                      WorkingSet* ws,
                                                      const Collection* collection,
                                                      Direction direction,
                                                      const RecordId& startLoc);

    /**
     * Returns a plan stage that is either an index scan or an index scan with a fetch stage.
     *
     * Used as a helper for indexScan() and deleteWithIndexScan().
     */
    static std::unique_ptr<PlanStage> _indexScan(OperationContext* opCtx,
                                                 WorkingSet* ws,
                                                 const Collection* collection,
                                                 const IndexDescriptor* descriptor,
                                                 const BSONObj& startKey,
                                                 const BSONObj& endKey,
                                                 BoundInclusion boundInclusion,
                                                 Direction direction = FORWARD,
                                                 int options = IXSCAN_DEFAULT);
};

}  // namespace mongo
