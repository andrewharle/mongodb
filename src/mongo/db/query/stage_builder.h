
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

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

class OperationContext;

/**
 * The StageBuilder converts a QuerySolution to an executable tree of PlanStage(s).
 */
class StageBuilder {
public:
    /**
     * Turns 'solution' into an executable tree of PlanStage(s).
     *
     * 'cq' must be the CanonicalQuery from which 'solution' is derived.
     *
     * Returns true if the PlanStage tree was built successfully.  The root of the tree is in
     * *rootOut and the WorkingSet that the tree uses is in wsIn.
     *
     * Returns false otherwise.  *rootOut and *wsOut are invalid.
     */
    static bool build(OperationContext* opCtx,
                      Collection* collection,
                      const CanonicalQuery& cq,
                      const QuerySolution& solution,
                      WorkingSet* wsIn,
                      PlanStage** rootOut);
};

}  // namespace mongo
