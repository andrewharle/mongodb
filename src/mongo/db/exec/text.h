
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

#include <memory>

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {

using fts::FTSQueryImpl;
using fts::FTSSpec;

class MatchExpression;
class OperationContext;

struct TextStageParams {
    TextStageParams(const FTSSpec& s) : spec(s) {}

    // Text index descriptor.  IndexCatalog owns this.
    IndexDescriptor* index;

    // Index spec.
    FTSSpec spec;

    // Index keys that precede the "text" index key.
    BSONObj indexPrefix;

    // The text query.
    FTSQueryImpl query;

    // True if we need the text score in the output, because the projection includes the 'textScore'
    // metadata field.
    bool wantTextScore = true;
};

/**
 * Implements a blocking stage that returns text search results.
 *
 * Output type: LOC_AND_OBJ.
 */
class TextStage final : public PlanStage {
public:
    TextStage(OperationContext* opCtx,
              const TextStageParams& params,
              WorkingSet* ws,
              const MatchExpression* filter);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() final;

    StageType stageType() const final {
        return STAGE_TEXT;
    }

    std::unique_ptr<PlanStageStats> getStats();

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    /**
     * Helper method to built the query execution plan for the text stage.
     */
    std::unique_ptr<PlanStage> buildTextTree(OperationContext* opCtx,
                                             WorkingSet* ws,
                                             const MatchExpression* filter,
                                             bool wantTextScore) const;

    // Parameters of this text stage.
    TextStageParams _params;

    // Stats.
    TextStats _specificStats;
};

}  // namespace mongo
