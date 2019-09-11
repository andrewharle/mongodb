// documentsourcetests.cpp : Unit tests for DocumentSource classes.

/**
 *    Copyright (C) 2012-2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/dbtests/dbtests.h"
namespace DocumentSourceCursorTests {

using boost::intrusive_ptr;
using std::unique_ptr;
using std::vector;

static const NamespaceString nss("unittests.documentsourcetests");
static const BSONObj metaTextScore = BSON("$meta"
                                          << "textScore");

BSONObj toBson(const intrusive_ptr<DocumentSource>& source) {
    vector<Value> arr;
    source->serializeToArray(arr);
    ASSERT_EQUALS(arr.size(), 1UL);
    return arr[0].getDocument().toBson();
}

class CollectionBase {
public:
    CollectionBase() : client(&_opCtx) {}

    ~CollectionBase() {
        client.dropCollection(nss.ns());
    }

protected:
    OperationContextImpl _opCtx;
    DBDirectClient client;
};

namespace DocumentSourceCursor {

using mongo::DocumentSourceCursor;

class Base : public CollectionBase {
public:
    Base() : _ctx(new ExpressionContext(&_opCtx, nss)) {
        _ctx->tempDir = storageGlobalParams.dbpath + "/_tmp";
    }

protected:
    void createSource() {
        // clean up first if this was called before
        _source.reset();
        _exec.reset();

        OldClientWriteContext ctx(&_opCtx, nss.ns());
        auto cq = uassertStatusOK(CanonicalQuery::canonicalize(nss, /*query=*/BSONObj()));
        _exec = uassertStatusOK(
            getExecutor(&_opCtx, ctx.getCollection(), std::move(cq), PlanExecutor::YIELD_MANUAL));

        _exec->saveState();
        _exec->registerExec();

        _source = DocumentSourceCursor::create(nss.ns(), _exec, _ctx);
    }
    intrusive_ptr<ExpressionContext> ctx() {
        return _ctx;
    }
    DocumentSourceCursor* source() {
        return _source.get();
    }

private:
    // It is important that these are ordered to ensure correct destruction order.
    std::shared_ptr<PlanExecutor> _exec;
    intrusive_ptr<ExpressionContext> _ctx;
    intrusive_ptr<DocumentSourceCursor> _source;
};

/** Create a DocumentSourceCursor. */
class Empty : public Base {
public:
    void run() {
        createSource();
        // The DocumentSourceCursor doesn't hold a read lock.
        ASSERT(!_opCtx.lockState()->isReadLocked());
        // The collection is empty, so the source produces no results.
        ASSERT(!source()->getNext());
        // Exhausting the source releases the read lock.
        ASSERT(!_opCtx.lockState()->isReadLocked());
    }
};

/** Iterate a DocumentSourceCursor. */
class Iterate : public Base {
public:
    void run() {
        client.insert(nss.ns(), BSON("a" << 1));
        createSource();
        // The DocumentSourceCursor doesn't hold a read lock.
        ASSERT(!_opCtx.lockState()->isReadLocked());
        // The cursor will produce the expected result.
        boost::optional<Document> next = source()->getNext();
        ASSERT(bool(next));
        ASSERT_EQUALS(Value(1), next->getField("a"));
        // There are no more results.
        ASSERT(!source()->getNext());
        // Exhausting the source releases the read lock.
        ASSERT(!_opCtx.lockState()->isReadLocked());
    }
};

/** Dispose of a DocumentSourceCursor. */
class Dispose : public Base {
public:
    void run() {
        createSource();
        // The DocumentSourceCursor doesn't hold a read lock.
        ASSERT(!_opCtx.lockState()->isReadLocked());
        source()->dispose();
        // Releasing the cursor releases the read lock.
        ASSERT(!_opCtx.lockState()->isReadLocked());
        // The source is marked as exhausted.
        ASSERT(!source()->getNext());
    }
};

/** Iterate a DocumentSourceCursor and then dispose of it. */
class IterateDispose : public Base {
public:
    void run() {
        client.insert(nss.ns(), BSON("a" << 1));
        client.insert(nss.ns(), BSON("a" << 2));
        client.insert(nss.ns(), BSON("a" << 3));
        createSource();
        // The result is as expected.
        boost::optional<Document> next = source()->getNext();
        ASSERT(bool(next));
        ASSERT_EQUALS(Value(1), next->getField("a"));
        // The next result is as expected.
        next = source()->getNext();
        ASSERT(bool(next));
        ASSERT_EQUALS(Value(2), next->getField("a"));
        // The DocumentSourceCursor doesn't hold a read lock.
        ASSERT(!_opCtx.lockState()->isReadLocked());
        source()->dispose();
        // Disposing of the source releases the lock.
        ASSERT(!_opCtx.lockState()->isReadLocked());
        // The source cannot be advanced further.
        ASSERT(!source()->getNext());
    }
};

/** Set a value or await an expected value. */
class PendingValue {
public:
    PendingValue(int initialValue) : _value(initialValue) {}
    void set(int newValue) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _value = newValue;
        _condition.notify_all();
    }
    void await(int expectedValue) const {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        while (_value != expectedValue) {
            _condition.wait(lk);
        }
    }

private:
    int _value;
    mutable stdx::mutex _mutex;
    mutable stdx::condition_variable _condition;
};


/** Test coalescing a limit into a cursor */
class LimitCoalesce : public Base {
public:
    intrusive_ptr<DocumentSourceLimit> mkLimit(long long limit) {
        return DocumentSourceLimit::create(ctx(), limit);
    }
    void run() {
        client.insert(nss.ns(), BSON("a" << 1));
        client.insert(nss.ns(), BSON("a" << 2));
        client.insert(nss.ns(), BSON("a" << 3));
        createSource();

        // initial limit becomes limit of cursor
        ASSERT(source()->coalesce(mkLimit(10)));
        ASSERT_EQUALS(source()->getLimit(), 10);

        // smaller limit lowers cursor limit
        ASSERT(source()->coalesce(mkLimit(2)));
        ASSERT_EQUALS(source()->getLimit(), 2);

        // higher limit doesn't effect cursor limit
        ASSERT(source()->coalesce(mkLimit(3)));
        ASSERT_EQUALS(source()->getLimit(), 2);

        // The cursor allows exactly 2 documents through
        ASSERT(bool(source()->getNext()));
        ASSERT(bool(source()->getNext()));
        ASSERT(!source()->getNext());
    }
};

}  // namespace DocumentSourceCursor

class All : public Suite {
public:
    All() : Suite("documentsource") {}
    void setupTests() {
        add<DocumentSourceCursor::Empty>();
        add<DocumentSourceCursor::Iterate>();
        add<DocumentSourceCursor::Dispose>();
        add<DocumentSourceCursor::IterateDispose>();
        add<DocumentSourceCursor::LimitCoalesce>();
    }
};

SuiteInstance<All> myall;

}  // namespace DocumentSourceCursorTests
