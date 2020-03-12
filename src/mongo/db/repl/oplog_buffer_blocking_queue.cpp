
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

#include "mongo/db/repl/oplog_buffer_blocking_queue.h"

namespace mongo {
namespace repl {

namespace {

// Limit buffer to 256MB
const size_t kOplogBufferSize = 256 * 1024 * 1024;

size_t getDocumentSize(const BSONObj& o) {
    // SERVER-9808 Avoid Fortify complaint about implicit signed->unsigned conversion
    return static_cast<size_t>(o.objsize());
}

}  // namespace

OplogBufferBlockingQueue::OplogBufferBlockingQueue() : _queue(kOplogBufferSize, &getDocumentSize) {}

void OplogBufferBlockingQueue::startup(OperationContext*) {}

void OplogBufferBlockingQueue::shutdown(OperationContext* opCtx) {
    clear(opCtx);
}

void OplogBufferBlockingQueue::pushEvenIfFull(OperationContext*, const Value& value) {
    invariant(!_drainMode);
    _queue.pushEvenIfFull(value);
    _notEmptyCv.notify_one();
}

void OplogBufferBlockingQueue::push(OperationContext*, const Value& value) {
    invariant(!_drainMode);
    _queue.push(value);
    _notEmptyCv.notify_one();
}

void OplogBufferBlockingQueue::pushAllNonBlocking(OperationContext*,
                                                  Batch::const_iterator begin,
                                                  Batch::const_iterator end) {
    invariant(!_drainMode);
    _queue.pushAllNonBlocking(begin, end);
    _notEmptyCv.notify_one();
}

void OplogBufferBlockingQueue::waitForSpace(OperationContext*, std::size_t size) {
    _queue.waitForSpace(size);
}

bool OplogBufferBlockingQueue::isEmpty() const {
    return _queue.empty();
}

std::size_t OplogBufferBlockingQueue::getMaxSize() const {
    return kOplogBufferSize;
}

std::size_t OplogBufferBlockingQueue::getSize() const {
    return _queue.size();
}

std::size_t OplogBufferBlockingQueue::getCount() const {
    return _queue.count();
}

void OplogBufferBlockingQueue::clear(OperationContext*) {
    _queue.clear();
}

bool OplogBufferBlockingQueue::tryPop(OperationContext*, Value* value) {
    return _queue.tryPop(*value);
}

bool OplogBufferBlockingQueue::waitForData(Seconds waitDuration) {
    Value ignored;
    stdx::unique_lock<stdx::mutex> lk(_notEmptyMutex);
    _notEmptyCv.wait_for(
        lk, waitDuration.toSystemDuration(), [&] { return _drainMode || _queue.peek(ignored); });
    return _queue.peek(ignored);
}

bool OplogBufferBlockingQueue::peek(OperationContext*, Value* value) {
    return _queue.peek(*value);
}

boost::optional<OplogBuffer::Value> OplogBufferBlockingQueue::lastObjectPushed(
    OperationContext*) const {
    return _queue.lastObjectPushed();
}

void OplogBufferBlockingQueue::enterDrainMode() {
    stdx::lock_guard<stdx::mutex> lk(_notEmptyMutex);
    _drainMode = true;
    _notEmptyCv.notify_one();
}

void OplogBufferBlockingQueue::exitDrainMode() {
    stdx::lock_guard<stdx::mutex> lk(_notEmptyMutex);
    _drainMode = false;
}

}  // namespace repl
}  // namespace mongo
