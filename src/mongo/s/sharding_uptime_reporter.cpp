
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

#include "mongo/s/sharding_uptime_reporter.h"

#include "mongo/db/client.h"
#include "mongo/db/server_options.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_mongos.h"
#include "mongo/s/grid.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostname_canonicalization.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/version.h"

namespace mongo {
namespace {

const Seconds kUptimeReportInterval(10);

std::string constructInstanceIdString(const std::string& hostName) {
    return str::stream() << hostName << ":" << serverGlobalParams.port;
}

/**
 * Reports the uptime status of the current instance to the config.pings collection. This method
 * is best-effort and never throws.
 */
void reportStatus(OperationContext* opCtx,
                  const std::string& instanceId,
                  const std::string& hostName,
                  const Timer& upTimeTimer) {
    MongosType mType;
    mType.setName(instanceId);
    mType.setPing(jsTime());
    mType.setUptime(upTimeTimer.seconds());
    // balancer is never active in mongos. Here for backwards compatibility only.
    mType.setWaiting(true);
    mType.setMongoVersion(VersionInfoInterface::instance().version().toString());
    mType.setAdvisoryHostFQDNs(
        getHostFQDNs(hostName, HostnameCanonicalizationMode::kForwardAndReverse));

    try {
        Grid::get(opCtx)
            ->catalogClient()
            ->updateConfigDocument(opCtx,
                                   MongosType::ConfigNS,
                                   BSON(MongosType::name(instanceId)),
                                   BSON("$set" << mType.toBSON()),
                                   true,
                                   ShardingCatalogClient::kMajorityWriteConcern)
            .status_with_transitional_ignore();
    } catch (const std::exception& e) {
        log() << "Caught exception while reporting uptime: " << e.what();
    }
}

}  // namespace

ShardingUptimeReporter::ShardingUptimeReporter() = default;

ShardingUptimeReporter::~ShardingUptimeReporter() {
    // The thread must not be running when this object is destroyed
    invariant(!_thread.joinable());
}

void ShardingUptimeReporter::startPeriodicThread() {
    invariant(!_thread.joinable());

    _thread = stdx::thread([] {
        Client::initThread("Uptime reporter");

        const std::string hostName(getHostNameCached());
        const std::string instanceId(constructInstanceIdString(hostName));
        const Timer upTimeTimer;

        while (!globalInShutdownDeprecated()) {
            {
                auto opCtx = cc().makeOperationContext();
                reportStatus(opCtx.get(), instanceId, hostName, upTimeTimer);

                auto status = Grid::get(opCtx.get())
                                  ->getBalancerConfiguration()
                                  ->refreshAndCheck(opCtx.get());
                if (!status.isOK()) {
                    warning() << "failed to refresh mongos settings" << causedBy(status);
                }
            }

            MONGO_IDLE_THREAD_BLOCK;
            sleepFor(kUptimeReportInterval);
        }
    });
}


}  // namespace mongo
