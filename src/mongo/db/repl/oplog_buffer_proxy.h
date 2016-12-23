/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include <boost/optional.hpp>
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
namespace repl {

class StorageInterface;

/**
 * Oplog buffer proxy that caches front and back (most recently pushed) oplog entries in the target
 * oplog buffer.
 */
class OplogBufferProxy : public OplogBuffer {
    MONGO_DISALLOW_COPYING(OplogBufferProxy);

public:
    explicit OplogBufferProxy(std::unique_ptr<OplogBuffer> target);

    /**
     * Returns target oplog buffer.
     */
    OplogBuffer* getTarget() const;

    void startup(OperationContext* txn) override;
    void shutdown(OperationContext* txn) override;
    void pushEvenIfFull(OperationContext* txn, const Value& value) override;
    void push(OperationContext* txn, const Value& value) override;
    void pushAllNonBlocking(OperationContext* txn,
                            Batch::const_iterator begin,
                            Batch::const_iterator end) override;
    void waitForSpace(OperationContext* txn, std::size_t size) override;
    bool isEmpty() const override;
    std::size_t getMaxSize() const override;
    std::size_t getSize() const override;
    std::size_t getCount() const override;
    void clear(OperationContext* txn) override;
    bool tryPop(OperationContext* txn, Value* value) override;
    bool waitForData(Seconds waitDuration) override;
    bool peek(OperationContext* txn, Value* value) override;
    boost::optional<Value> lastObjectPushed(OperationContext* txn) const override;

    // ---- Testing API ----
    boost::optional<Value> getLastPeeked_forTest() const;

private:
    // Target oplog buffer. Owned by us.
    std::unique_ptr<OplogBuffer> _target;

    // If both mutexes have to be acquired, acquire _lastPushedMutex first.
    mutable stdx::mutex _lastPushedMutex;
    boost::optional<Value> _lastPushed;

    mutable stdx::mutex _lastPeekedMutex;
    boost::optional<Value> _lastPeeked;
};

}  // namespace repl
}  // namespace mongo
