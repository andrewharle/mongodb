
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
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/stats/counters.h"

namespace mongo {

/**
 * A semi-parsed version of a Pipeline, parsed just enough to determine information like what
 * foreign collections are involved.
 */
class LiteParsedPipeline {
public:
    /**
     * Constructs a LiteParsedPipeline from the raw BSON stages given in 'request'.
     *
     * May throw a AssertionException if there is an invalid stage specification, although full
     * validation happens later, during Pipeline construction.
     */
    LiteParsedPipeline(const AggregationRequest& request) : _nss(request.getNamespaceString()) {
        _stageSpecs.reserve(request.getPipeline().size());

        for (auto&& rawStage : request.getPipeline()) {
            _stageSpecs.push_back(LiteParsedDocumentSource::parse(request, rawStage));
        }
    }

    /**
     * Returns all foreign namespaces referenced by stages within this pipeline, if any.
     */
    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const {
        stdx::unordered_set<NamespaceString> involvedNamespaces;
        for (auto&& spec : _stageSpecs) {
            auto stagesInvolvedNamespaces = spec->getInvolvedNamespaces();
            involvedNamespaces.insert(stagesInvolvedNamespaces.begin(),
                                      stagesInvolvedNamespaces.end());
        }
        return involvedNamespaces;
    }

    /**
     * Returns a list of the priviliges required for this pipeline.
     */
    PrivilegeVector requiredPrivileges(bool isMongos) const {
        PrivilegeVector requiredPrivileges;
        for (auto&& spec : _stageSpecs) {
            Privilege::addPrivilegesToPrivilegeVector(&requiredPrivileges,
                                                      spec->requiredPrivileges(isMongos));
        }

        return requiredPrivileges;
    }

    /**
     * Returns true if the pipeline begins with a $collStats stage.
     */
    bool startsWithCollStats() const {
        return !_stageSpecs.empty() && _stageSpecs.front()->isCollStats();
    }

    /**
     * Returns true if the pipeline has a $changeStream stage.
     */
    bool hasChangeStream() const {
        return std::any_of(_stageSpecs.begin(), _stageSpecs.end(), [](auto&& spec) {
            return spec->isChangeStream();
        });
    }

    /**
     * Returns true if this pipeline's UUID and collation should be resolved. For the latter, this
     * means adopting the collection's default collation, unless a custom collation was specified.
     */
    bool shouldResolveUUIDAndCollation() const {
        // Collectionless aggregations do not have a UUID or default collation.
        return !_nss.isCollectionlessAggregateNS() &&
            std::all_of(_stageSpecs.begin(), _stageSpecs.end(), [](auto&& spec) {
                return spec->shouldResolveUUIDAndCollation();
            });
    }

    /**
     * Returns false if the pipeline has any stage which must be run locally on mongos.
     */
    bool allowedToForwardFromMongos() const {
        return std::all_of(_stageSpecs.cbegin(), _stageSpecs.cend(), [](const auto& spec) {
            return spec->allowedToForwardFromMongos();
        });
    }

    /**
     * Returns false if the pipeline has any Documet Source which requires rewriting via serialize.
     */
    bool allowedToPassthroughFromMongos() const {
        return std::all_of(_stageSpecs.cbegin(), _stageSpecs.cend(), [](const auto& spec) {
            return spec->allowedToPassthroughFromMongos();
        });
    }

    /**
     * Verifies that this pipeline is allowed to run with the specified read concern. This ensures
     * that each stage is compatible, and throws a UserException if not.
     */
    void assertSupportsReadConcern(OperationContext* opCtx,
                                   boost::optional<ExplainOptions::Verbosity> explain) const {
        auto readConcern = repl::ReadConcernArgs::get(opCtx);

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Explain for the aggregate command cannot run with a readConcern "
                              << "other than 'local', or in a multi-document transaction. Current "
                              << "readConcern: "
                              << readConcern.toString(),
                !explain || readConcern.getLevel() == repl::ReadConcernLevel::kLocalReadConcern);

        for (auto&& spec : _stageSpecs) {
            spec->assertSupportsReadConcern(readConcern);
        }
    }

    /**
     * Increments global stage counters corresponding to the stages in this lite parsed pipeline.
     */
    void tickGlobalStageCounters() const {
        for (auto&& stage : _stageSpecs) {
            // Tick counter corresponding to current stage.
            auto entry = aggStageCounters.stageCounterMap.find(stage->getParseTimeName());
            invariant(entry != aggStageCounters.stageCounterMap.end());
            entry->second->counter.increment(1);

            // Recursively step through any sub-pipelines.
            for (auto&& subPipeline : stage->getSubPipelines()) {
                subPipeline.tickGlobalStageCounters();
            }
        }
    }

private:
    std::vector<std::unique_ptr<LiteParsedDocumentSource>> _stageSpecs;
    NamespaceString _nss;
};

}  // namespace mongo
