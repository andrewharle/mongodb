/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/service_context_noop.h"

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/op_observer.h"
#include "mongo/stdx/memory.h"

namespace mongo {

StorageEngine* ServiceContextNoop::getGlobalStorageEngine() {
    return NULL;
}

void ServiceContextNoop::initializeGlobalStorageEngine() {}

void ServiceContextNoop::shutdownGlobalStorageEngineCleanly() {}

void ServiceContextNoop::registerStorageEngine(const std::string& name,
                                               const StorageEngine::Factory* factory) {
    // Takes ownership of 'factory' and deletes it because we don't need it.
    delete factory;
}

bool ServiceContextNoop::isRegisteredStorageEngine(const std::string& name) {
    return false;
}

StorageFactoriesIterator* ServiceContextNoop::makeStorageFactoriesIterator() {
    class EmptySFI : public StorageFactoriesIterator {
    public:
        virtual bool more() const {
            return false;
        }
        virtual const StorageEngine::Factory* next() {
            invariant(false);
        }
    };
    return new EmptySFI();
}

void ServiceContextNoop::setKillAllOperations() {}

void ServiceContextNoop::unsetKillAllOperations() {}

bool ServiceContextNoop::getKillAllOperations() {
    return false;
}

bool ServiceContextNoop::killOperation(unsigned int opId) {
    return false;
}

void ServiceContextNoop::killAllUserOperations(const OperationContext* txn) {}

void ServiceContextNoop::registerKillOpListener(KillOpListenerInterface* listener) {}

std::unique_ptr<OperationContext> ServiceContextNoop::_newOpCtx(Client* client) {
    return stdx::make_unique<OperationContextNoop>(client, _nextOpId.fetchAndAdd(1));
}

void ServiceContextNoop::setOpObserver(std::unique_ptr<OpObserver> opObserver) {}

OpObserver* ServiceContextNoop::getOpObserver() {
    return nullptr;
}
}  // namespace mongo
