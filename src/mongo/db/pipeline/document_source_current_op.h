
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

#include "mongo/db/pipeline/document_source.h"

namespace mongo {

class DocumentSourceCurrentOp final : public DocumentSource {
public:
    using TruncationMode = MongoProcessInterface::CurrentOpTruncateMode;
    using ConnMode = MongoProcessInterface::CurrentOpConnectionsMode;
    using LocalOpsMode = MongoProcessInterface::CurrentOpLocalOpsMode;
    using SessionMode = MongoProcessInterface::CurrentOpSessionsMode;
    using UserMode = MongoProcessInterface::CurrentOpUserMode;

    static constexpr StringData kStageName = "$currentOp"_sd;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const AggregationRequest& request,
                                                 const BSONElement& spec);

        LiteParsed(UserMode allUsers, LocalOpsMode localOps)
            : _allUsers(allUsers), _localOps(localOps) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos) const final {
            PrivilegeVector privileges;

            // In a sharded cluster, we always need the inprog privilege to run $currentOp on the
            // shards. If we are only looking up local mongoS operations, we do not need inprog to
            // view our own ops but *do* require it to view other users' ops.
            if (_allUsers == UserMode::kIncludeAll ||
                (isMongos && _localOps == LocalOpsMode::kRemoteShardOps)) {
                privileges.push_back({ResourcePattern::forClusterResource(), ActionType::inprog});
            }

            return privileges;
        }

        bool allowedToForwardFromMongos() const final {
            return _localOps == LocalOpsMode::kRemoteShardOps;
        }

        bool allowedToPassthroughFromMongos() const final {
            return _localOps == LocalOpsMode::kRemoteShardOps;
        }

        bool isInitialSource() const final {
            return true;
        }

        void assertSupportsReadConcern(const repl::ReadConcernArgs& readConcern) const {
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "Aggregation stage " << kStageName << " cannot run with a "
                                  << "readConcern other than 'local', or in a multi-document "
                                  << "transaction. Current readConcern: "
                                  << readConcern.toString(),
                    readConcern.getLevel() == repl::ReadConcernLevel::kLocalReadConcern);
        }

    private:
        const UserMode _allUsers;
        const LocalOpsMode _localOps;
    };

    static boost::intrusive_ptr<DocumentSourceCurrentOp> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        ConnMode includeIdleConnections = ConnMode::kExcludeIdle,
        SessionMode includeIdleSessions = SessionMode::kIncludeIdle,
        UserMode includeOpsFromAllUsers = UserMode::kExcludeOthers,
        LocalOpsMode showLocalOpsOnMongoS = LocalOpsMode::kRemoteShardOps,
        TruncationMode truncateOps = TruncationMode::kNoTruncation);

    GetNextResult getNext() final;

    const char* getSourceName() const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     (_showLocalOpsOnMongoS == LocalOpsMode::kLocalMongosOps
                                          ? HostTypeRequirement::kLocalOnly
                                          : HostTypeRequirement::kAnyShard),
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed);

        constraints.isIndependentOfAnyCollection = true;
        constraints.requiresInputDocSource = false;
        return constraints;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

private:
    DocumentSourceCurrentOp(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                            ConnMode includeIdleConnections,
                            SessionMode includeIdleSessions,
                            UserMode includeOpsFromAllUsers,
                            LocalOpsMode showLocalOpsOnMongoS,
                            TruncationMode truncateOps)
        : DocumentSource(pExpCtx),
          _includeIdleConnections(includeIdleConnections),
          _includeIdleSessions(includeIdleSessions),
          _includeOpsFromAllUsers(includeOpsFromAllUsers),
          _showLocalOpsOnMongoS(showLocalOpsOnMongoS),
          _truncateOps(truncateOps) {}

    ConnMode _includeIdleConnections = ConnMode::kExcludeIdle;
    SessionMode _includeIdleSessions = SessionMode::kIncludeIdle;
    UserMode _includeOpsFromAllUsers = UserMode::kExcludeOthers;
    LocalOpsMode _showLocalOpsOnMongoS = LocalOpsMode::kRemoteShardOps;
    TruncationMode _truncateOps = TruncationMode::kNoTruncation;

    std::string _shardName;

    std::vector<BSONObj> _ops;
    std::vector<BSONObj>::iterator _opsIter;
};

}  // namespace mongo
