
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

#include "mongo/platform/basic.h"

#include "mongo/rpc/metadata/client_metadata_ismaster.h"

#include <string>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace {

const auto getClientMetadataIsMasterState =
    Client::declareDecoration<ClientMetadataIsMasterState>();

}  // namespace

ClientMetadataIsMasterState& ClientMetadataIsMasterState::get(Client* client) {
    return getClientMetadataIsMasterState(*client);
}

bool ClientMetadataIsMasterState::hasSeenIsMaster() const {
    return _hasSeenIsMaster;
}

void ClientMetadataIsMasterState::setSeenIsMaster() {
    invariant(!_hasSeenIsMaster);
    _hasSeenIsMaster = true;
}

const boost::optional<ClientMetadata>& ClientMetadataIsMasterState::getClientMetadata() const {
    return _clientMetadata;
}

void ClientMetadataIsMasterState::setClientMetadata(Client* client,
                                                    boost::optional<ClientMetadata> clientMetadata,
                                                    bool setViaMetadata) {
    auto& state = get(client);

    stdx::lock_guard<Client> lk(*client);
    state._clientMetadata = std::move(clientMetadata);
    state._setViaMetadata = setViaMetadata;
}


Status ClientMetadataIsMasterState::readFromMetadata(OperationContext* opCtx,
                                                     BSONElement& element) {
    auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx->getClient());

    // If client metadata is not present in network requests, reset the in-memory metadata to be
    // blank so that the wrong
    // app name is not propagated.
    if (element.eoo()) {
        auto client = opCtx->getClient();

        if (clientMetadataIsMasterState._setViaMetadata && !client->isInDirectClient()) {
            clientMetadataIsMasterState.setClientMetadata(client, boost::none, true);
        }

        return Status::OK();
    }

    auto swParseClientMetadata = ClientMetadata::parse(element);

    if (!swParseClientMetadata.getStatus().isOK()) {
        return swParseClientMetadata.getStatus();
    }

    clientMetadataIsMasterState.setClientMetadata(
        opCtx->getClient(), std::move(swParseClientMetadata.getValue()), true);

    return Status::OK();
}

void ClientMetadataIsMasterState::writeToMetadata(OperationContext* opCtx,
                                                  BSONObjBuilder* builder) {
    // We may be asked to write metadata on background threads that are not associated with an
    // operation context
    if (!opCtx) {
        return;
    }

    const auto& clientMetadata =
        ClientMetadataIsMasterState::get(opCtx->getClient()).getClientMetadata();

    // Skip appending metadata if there is none
    if (!clientMetadata || clientMetadata.get().getDocument().isEmpty()) {
        return;
    }

    builder->append(ClientMetadata::fieldName(), clientMetadata.get().getDocument());
}

}  // namespace mongo
