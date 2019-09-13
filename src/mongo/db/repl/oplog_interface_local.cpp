
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

#include "mongo/db/repl/oplog_interface_local.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {
namespace repl {

namespace {

class OplogIteratorLocal : public OplogInterface::Iterator {
public:
    OplogIteratorLocal(OperationContext* opCtx, const std::string& collectionName);

    StatusWith<Value> next() override;

private:
    Lock::DBLock _dbLock;
    Lock::CollectionLock _collectionLock;
    OldClientContext _ctx;
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _exec;
};

OplogIteratorLocal::OplogIteratorLocal(OperationContext* opCtx, const std::string& collectionName)
    : _dbLock(opCtx, nsToDatabase(collectionName), MODE_IS),
      _collectionLock(opCtx->lockState(), collectionName, MODE_S),
      _ctx(opCtx, collectionName),
      _exec(InternalPlanner::collectionScan(opCtx,
                                            collectionName,
                                            _ctx.db()->getCollection(opCtx, collectionName),
                                            PlanExecutor::NO_YIELD,
                                            InternalPlanner::BACKWARD)) {}

StatusWith<OplogInterface::Iterator::Value> OplogIteratorLocal::next() {
    BSONObj obj;
    RecordId recordId;

    PlanExecutor::ExecState state;
    if (PlanExecutor::ADVANCED != (state = _exec->getNext(&obj, &recordId))) {
        return StatusWith<Value>(ErrorCodes::CollectionIsEmpty,
                                 "no more operations in local oplog");
    }

    // Non-yielding collection scans from InternalPlanner will never error.
    invariant(PlanExecutor::ADVANCED == state || PlanExecutor::IS_EOF == state);

    return StatusWith<Value>(std::make_pair(obj, recordId));
}

}  // namespace

OplogInterfaceLocal::OplogInterfaceLocal(OperationContext* opCtx, const std::string& collectionName)
    : _opCtx(opCtx), _collectionName(collectionName) {
    invariant(opCtx);
    invariant(!collectionName.empty());
}

std::string OplogInterfaceLocal::toString() const {
    return str::stream() << "LocalOplogInterface: "
                            "operation context: "
                         << _opCtx->getOpID() << "; collection: " << _collectionName;
}

std::unique_ptr<OplogInterface::Iterator> OplogInterfaceLocal::makeIterator() const {
    return std::unique_ptr<OplogInterface::Iterator>(
        new OplogIteratorLocal(_opCtx, _collectionName));
}

HostAndPort OplogInterfaceLocal::hostAndPort() const {
    return {getHostNameCached(), serverGlobalParams.port};
}

}  // namespace repl
}  // namespace mongo
