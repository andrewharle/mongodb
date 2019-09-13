
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

#include "mongo/db/exec/skip.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

// static
const char* SkipStage::kStageType = "SKIP";

SkipStage::SkipStage(OperationContext* opCtx, long long toSkip, WorkingSet* ws, PlanStage* child)
    : PlanStage(kStageType, opCtx), _ws(ws), _toSkip(toSkip) {
    _children.emplace_back(child);
}

SkipStage::~SkipStage() {}

bool SkipStage::isEOF() {
    return child()->isEOF();
}

PlanStage::StageState SkipStage::doWork(WorkingSetID* out) {
    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState status = child()->work(&id);

    if (PlanStage::ADVANCED == status) {
        // If we're still skipping results...
        if (_toSkip > 0) {
            // ...drop the result.
            --_toSkip;
            _ws->free(id);
            return PlanStage::NEED_TIME;
        }

        *out = id;
        return PlanStage::ADVANCED;
    } else if (PlanStage::FAILURE == status || PlanStage::DEAD == status) {
        // The stage which produces a failure is responsible for allocating a working set member
        // with error details.
        invariant(WorkingSet::INVALID_ID != id);
        *out = id;
        return status;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    // NEED_TIME, NEED_YIELD, ERROR, IS_EOF
    return status;
}

unique_ptr<PlanStageStats> SkipStage::getStats() {
    _commonStats.isEOF = isEOF();
    _specificStats.skip = _toSkip;
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_SKIP);
    ret->specific = make_unique<SkipStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* SkipStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
