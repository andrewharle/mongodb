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

#include "mongo/s/shard_util.h"

#include "mongo/base/status_with.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace shardutil {

StatusWith<long long> retrieveTotalShardSize(OperationContext* txn,
                                             ShardId shardId,
                                             ShardRegistry* shardRegistry) {
    auto listDatabasesStatus =
        shardRegistry->runCommandOnShard(txn,
                                         shardId,
                                         ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
                                         "admin",
                                         BSON("listDatabases" << 1));
    if (!listDatabasesStatus.isOK()) {
        return listDatabasesStatus.getStatus();
    }

    BSONElement totalSizeElem = listDatabasesStatus.getValue()["totalSize"];
    if (!totalSizeElem.isNumber()) {
        return {ErrorCodes::NoSuchKey, "totalSize field not found in listDatabases"};
    }

    return totalSizeElem.numberLong();
}

}  // namespace shardutil
}  // namespace mongo
