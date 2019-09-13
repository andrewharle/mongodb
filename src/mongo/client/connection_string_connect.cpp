
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/connection_string.h"

#include <list>
#include <memory>

#include "mongo/client/dbclient_rs.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

stdx::mutex ConnectionString::_connectHookMutex;
ConnectionString::ConnectionHook* ConnectionString::_connectHook = NULL;

std::unique_ptr<DBClientBase> ConnectionString::connect(StringData applicationName,
                                                        std::string& errmsg,
                                                        double socketTimeout,
                                                        const MongoURI* uri) const {
    MongoURI newURI{};
    if (uri) {
        newURI = *uri;
    }

    switch (_type) {
        case MASTER: {
            for (const auto& server : _servers) {
                auto c = stdx::make_unique<DBClientConnection>(true, 0, newURI);

                c->setSoTimeout(socketTimeout);
                LOG(1) << "creating new connection to:" << server;
                if (!c->connect(server, applicationName, errmsg)) {
                    continue;
                }
                LOG(1) << "connected connection!";
                return std::move(c);
            }
            return nullptr;
        }

        case SET: {
            auto set = stdx::make_unique<DBClientReplicaSet>(
                _setName, _servers, applicationName, socketTimeout, std::move(newURI));
            if (!set->connect()) {
                errmsg = "connect failed to replica set ";
                errmsg += toString();
                return nullptr;
            }
            return std::move(set);
        }

        case CUSTOM: {
            // Lock in case other things are modifying this at the same time
            stdx::lock_guard<stdx::mutex> lk(_connectHookMutex);

            // Allow the replacement of connections with other connections - useful for testing.

            uassert(16335,
                    "custom connection to " + this->toString() +
                        " specified with no connection hook",
                    _connectHook);

            // Double-checked lock, since this will never be active during normal operation
            auto replacementConn = _connectHook->connect(*this, errmsg, socketTimeout);

            log() << "replacing connection to " << this->toString() << " with "
                  << (replacementConn ? replacementConn->getServerAddress() : "(empty)");

            return replacementConn;
        }

        case LOCAL:
        case INVALID:
            MONGO_UNREACHABLE;
    }

    MONGO_UNREACHABLE;
}

}  // namepspace mongo
