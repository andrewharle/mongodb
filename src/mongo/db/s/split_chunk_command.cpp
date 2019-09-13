
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/split_chunk.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;

namespace {

class SplitChunkCommand : public ErrmsgCommandDeprecated {
public:
    SplitChunkCommand() : ErrmsgCommandDeprecated("splitChunk") {}

    std::string help() const override {
        return "internal command usage only\n"
               "example:\n"
               " { splitChunk:\"db.foo\" , keyPattern: {a:1} , min : {a:100} , max: {a:200} { "
               "splitKeys : [ {a:150} , ... ]}";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

        const NamespaceString nss = NamespaceString(parseNs(dbname, cmdObj));

        // Check whether parameters passed to splitChunk are sound
        BSONObj keyPatternObj;
        {
            BSONElement keyPatternElem;
            auto keyPatternStatus =
                bsonExtractTypedField(cmdObj, "keyPattern", Object, &keyPatternElem);

            if (!keyPatternStatus.isOK()) {
                errmsg = "need to specify the key pattern the collection is sharded over";
                return false;
            }
            keyPatternObj = keyPatternElem.Obj();
        }

        auto chunkRange = uassertStatusOK(ChunkRange::fromBSON(cmdObj));

        string shardName;
        auto parseShardNameStatus = bsonExtractStringField(cmdObj, "from", &shardName);
        uassertStatusOK(parseShardNameStatus);

        log() << "received splitChunk request: " << redact(cmdObj);

        vector<BSONObj> splitKeys;
        {
            BSONElement splitKeysElem;
            auto splitKeysElemStatus =
                bsonExtractTypedField(cmdObj, "splitKeys", mongo::Array, &splitKeysElem);

            if (!splitKeysElemStatus.isOK()) {
                errmsg = "need to provide the split points to chunk over";
                return false;
            }
            BSONObjIterator it(splitKeysElem.Obj());
            while (it.more()) {
                splitKeys.push_back(it.next().Obj().getOwned());
            }
        }

        OID expectedCollectionEpoch;
        uassertStatusOK(bsonExtractOIDField(cmdObj, "epoch", &expectedCollectionEpoch));

        auto statusWithOptionalChunkRange = splitChunk(
            opCtx, nss, keyPatternObj, chunkRange, splitKeys, shardName, expectedCollectionEpoch);

        // If the split chunk returns something that is not Status::Ok(), then something failed.
        uassertStatusOK(statusWithOptionalChunkRange.getStatus());

        // Otherwise, we want to check whether or not top-chunk optimization should be performed.
        // If yes, then we should have a ChunkRange that was returned. Regardless of whether it
        // should be performed, we will return true.
        if (auto topChunk = statusWithOptionalChunkRange.getValue()) {
            result.append("shouldMigrate",
                          BSON("min" << topChunk->getMin() << "max" << topChunk->getMax()));
        }
        return true;
    }

} cmdSplitChunk;

}  // namespace
}  // namespace mongo
