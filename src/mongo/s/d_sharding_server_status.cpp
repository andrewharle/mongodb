/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

namespace mongo {

namespace {

class ShardingServerStatus : public ServerStatusSection {
public:
    ShardingServerStatus();

    bool includeByDefault() const final;

    BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const final;
};

}  // namespace

ShardingServerStatus shardingServerStatus;

ShardingServerStatus::ShardingServerStatus() : ServerStatusSection("sharding") {}

bool ShardingServerStatus::includeByDefault() const {
    return true;
}

// This implementation runs on mongoD.
BSONObj ShardingServerStatus::generateSection(OperationContext* txn,
                                              const BSONElement& configElement) const {
    ShardingState* shardingState = ShardingState::get(txn);
    if (!shardingState->enabled()) {
        return BSONObj();
    }
    BSONObjBuilder result;
    result.append("configsvrConnectionString", shardingState->getConfigServer(txn).toString());

    auto catalogManager = grid.catalogManager(txn);
    if (catalogManager->getMode() == CatalogManager::ConfigServerMode::CSRS) {
        grid.shardRegistry()->getConfigOpTime().append(&result, "lastSeenConfigServerOpTime");
    }

    return result.obj();
}

}  // namespace mongo
