
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

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_test_help.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/barrier.h"
#include "mongo/util/concurrency/thread_pool.h"


namespace mongo {
namespace repl {
namespace {

class OplogTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
};

void OplogTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    auto opCtx = cc().makeOperationContext();

    // Set up ReplicationCoordinator and create oplog.
    ReplicationCoordinator::set(service, stdx::make_unique<ReplicationCoordinatorMock>(service));
    setOplogCollectionName(service);
    createOplog(opCtx.get());

    // Ensure that we are primary.
    auto replCoord = ReplicationCoordinator::get(opCtx.get());
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));
}

/**
 * Assert that oplog only has a single entry and return that oplog entry.
 */
OplogEntry _getSingleOplogEntry(OperationContext* opCtx) {
    OplogInterfaceLocal oplogInterface(opCtx, NamespaceString::kRsOplogNamespace.ns());
    auto oplogIter = oplogInterface.makeIterator();
    auto opEntry = unittest::assertGet(oplogIter->next());
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus())
        << "Expected only 1 document in the oplog collection " << NamespaceString::kRsOplogNamespace
        << " but found more than 1 document instead";
    return unittest::assertGet(OplogEntry::parse(opEntry.first));
}

TEST_F(OplogTest, LogOpReturnsOpTimeOnSuccessfulInsertIntoOplogCollection) {
    auto opCtx = cc().makeOperationContext();

    const NamespaceString nss("test.coll");
    auto msgObj = BSON("msg"
                       << "hello, world!");

    // Write to the oplog.
    OpTime opTime;
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opTime = logOp(opCtx.get(),
                       "n",
                       nss,
                       {},
                       msgObj,
                       nullptr,
                       false,
                       Date_t::now(),
                       {},
                       kUninitializedStmtId,
                       {},
                       OplogSlot());
        ASSERT_FALSE(opTime.isNull());
        wunit.commit();
    }

    OplogEntry oplogEntry = _getSingleOplogEntry(opCtx.get());

    // Ensure that msg fields were properly added to the oplog entry.
    ASSERT_EQUALS(opTime, oplogEntry.getOpTime())
        << "OpTime returned from logOp() did not match that in the oplog entry written to the "
           "oplog: "
        << oplogEntry.toBSON();
    ASSERT(OpTypeEnum::kNoop == oplogEntry.getOpType()) << "Expected 'n' op type but found '"
                                                        << OpType_serializer(oplogEntry.getOpType())
                                                        << "' instead: " << oplogEntry.toBSON();
    ASSERT_BSONOBJ_EQ(msgObj, oplogEntry.getObject());

    // Ensure that the msg optime returned is the same as the last optime in the ReplClientInfo.
    ASSERT_EQUALS(ReplClientInfo::forClient(&cc()).getLastOp(), opTime);
}

/**
 * Checks optime and namespace in oplog entry.
 */
void _checkOplogEntry(const OplogEntry& oplogEntry,
                      const OpTime& expectedOpTime,
                      const NamespaceString& expectedNss) {
    ASSERT_EQUALS(expectedOpTime, oplogEntry.getOpTime()) << oplogEntry.toBSON();
    ASSERT_EQUALS(expectedNss, oplogEntry.getNamespace()) << oplogEntry.toBSON();
}
void _checkOplogEntry(const OplogEntry& oplogEntry,
                      const std::pair<OpTime, NamespaceString>& expectedOpTimeAndNss) {
    _checkOplogEntry(oplogEntry, expectedOpTimeAndNss.first, expectedOpTimeAndNss.second);
}

/**
 * Test function that schedules two concurrent logOp() tasks using a thread pool.
 * Checks the state of the oplog collection against the optimes returned from logOp().
 * Before returning, updates 'opTimeNssMap' with the optimes from logOp() and 'oplogEntries' with
 * the contents of the oplog collection.
 */
using OpTimeNamespaceStringMap = std::map<OpTime, NamespaceString>;
using MakeTaskFunction =
    stdx::function<ThreadPoolInterface::Task(const NamespaceString& nss,
                                             stdx::mutex* mtx,
                                             OpTimeNamespaceStringMap* opTimeNssMap,
                                             unittest::Barrier* barrier)>;
void _testConcurrentLogOp(const MakeTaskFunction& makeTaskFunction,
                          OpTimeNamespaceStringMap* opTimeNssMap,
                          std::vector<OplogEntry>* oplogEntries,
                          std::size_t expectedNumOplogEntries) {
    ASSERT_LESS_THAN_OR_EQUALS(expectedNumOplogEntries, 2U);

    // Run 2 concurrent logOp() requests using the thread pool.
    ThreadPool::Options options;
    options.maxThreads = 2U;
    options.onCreateThread = [](const std::string& name) { Client::initThread(name); };
    ThreadPool pool(options);
    pool.startup();

    // Run 2 concurrent logOp() requests using the thread pool.
    // Use a barrier with a thread count of 3 to ensure both logOp() tasks are complete before this
    // test thread can proceed with shutting the thread pool down.
    stdx::mutex mtx;
    unittest::Barrier barrier(3U);
    const NamespaceString nss1("test1.coll");
    const NamespaceString nss2("test2.coll");
    ASSERT_OK(pool.schedule(makeTaskFunction(nss1, &mtx, opTimeNssMap, &barrier)))
        << "Failed to schedule logOp() task for namespace " << nss1;
    ASSERT_OK(pool.schedule(makeTaskFunction(nss2, &mtx, opTimeNssMap, &barrier)))
        << "Failed to schedule logOp() task for namespace " << nss2;
    barrier.countDownAndWait();

    // Shut thread pool down.
    pool.shutdown();
    pool.join();

    // Read oplog entries from the oplog collection starting with the entry with the most recent
    // optime.
    auto opCtx = cc().makeOperationContext();
    OplogInterfaceLocal oplogInterface(opCtx.get(), NamespaceString::kRsOplogNamespace.ns());
    auto oplogIter = oplogInterface.makeIterator();
    auto nextValue = oplogIter->next();
    while (nextValue.isOK()) {
        const auto& doc = nextValue.getValue().first;
        oplogEntries->emplace_back(unittest::assertGet(OplogEntry::parse(doc)));
        nextValue = oplogIter->next();
    }
    ASSERT_EQUALS(expectedNumOplogEntries, oplogEntries->size());

    // Reverse 'oplogEntries' because we inserted the oplog entries in descending order by optime.
    std::reverse(oplogEntries->begin(), oplogEntries->end());

    // Look up namespaces and their respective optimes (returned by logOp()) in the map.
    stdx::lock_guard<stdx::mutex> lock(mtx);
    ASSERT_EQUALS(2U, opTimeNssMap->size());
}

/**
 * Inserts noop oplog entry with embedded namespace string.
 * Inserts optime/namespace pair into map while holding a lock on the mutex.
 * Returns optime of generated oplog entry.
 */
OpTime _logOpNoopWithMsg(OperationContext* opCtx,
                         stdx::mutex* mtx,
                         OpTimeNamespaceStringMap* opTimeNssMap,
                         const NamespaceString& nss) {
    stdx::lock_guard<stdx::mutex> lock(*mtx);

    // logOp() must be called while holding lock because ephemeralForTest storage engine does not
    // support concurrent updates to its internal state.
    const auto msgObj = BSON("msg" << nss.ns());
    auto opTime = logOp(opCtx,
                        "n",
                        nss,
                        {},
                        msgObj,
                        nullptr,
                        false,
                        Date_t::now(),
                        {},
                        kUninitializedStmtId,
                        {},
                        OplogSlot());
    ASSERT_FALSE(opTime.isNull());

    ASSERT(opTimeNssMap->find(opTime) == opTimeNssMap->end())
        << "Unable to add namespace " << nss << " to map - map contains duplicate entry for optime "
        << opTime;
    opTimeNssMap->insert(std::make_pair(opTime, nss));

    return opTime;
}

TEST_F(OplogTest, ConcurrentLogOpWithoutDocLockingSupport) {
    OpTimeNamespaceStringMap opTimeNssMap;
    std::vector<OplogEntry> oplogEntries;

    _testConcurrentLogOp(
        [](const NamespaceString& nss,
           stdx::mutex* mtx,
           OpTimeNamespaceStringMap* opTimeNssMap,
           unittest::Barrier* barrier) {
            return [=] {
                auto opCtx = cc().makeOperationContext();
                AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
                WriteUnitOfWork wunit(opCtx.get());

                _logOpNoopWithMsg(opCtx.get(), mtx, opTimeNssMap, nss);

                // In a storage engine that does not support doc locking, upon returning from
                // logOp(), this thread still holds an implicit MODE_X lock on the oplog collection
                // until it commits the WriteUnitOfWork. Therefore, we must wait on the barrier
                // after the WUOW is committed.
                wunit.commit();
                barrier->countDownAndWait();
            };
        },
        &opTimeNssMap,
        &oplogEntries,
        2U);

    _checkOplogEntry(oplogEntries[0], *(opTimeNssMap.begin()));
    _checkOplogEntry(oplogEntries[1], *(opTimeNssMap.rbegin()));
}

TEST_F(OplogTest, ConcurrentLogOpWithDocLockingSupport) {
    OpTimeNamespaceStringMap opTimeNssMap;
    std::vector<OplogEntry> oplogEntries;

    ForceSupportsDocLocking support(true);
    _testConcurrentLogOp(
        [](const NamespaceString& nss,
           stdx::mutex* mtx,
           OpTimeNamespaceStringMap* opTimeNssMap,
           unittest::Barrier* barrier) {
            return [=] {
                auto opCtx = cc().makeOperationContext();
                AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
                WriteUnitOfWork wunit(opCtx.get());

                _logOpNoopWithMsg(opCtx.get(), mtx, opTimeNssMap, nss);

                // In a storage engine that supports doc locking, it is okay for multiple threads to
                // maintain uncommitted WUOWs upon returning from logOp() because each thread will
                // hold an implicit MODE_IX lock on the oplog collection.
                barrier->countDownAndWait();
                wunit.commit();
            };
        },
        &opTimeNssMap,
        &oplogEntries,
        2U);

    _checkOplogEntry(oplogEntries[0], *(opTimeNssMap.begin()));
    _checkOplogEntry(oplogEntries[1], *(opTimeNssMap.rbegin()));
}

TEST_F(OplogTest, ConcurrentLogOpWithDocLockingSupportRevertFirstOplogEntry) {
    OpTimeNamespaceStringMap opTimeNssMap;
    std::vector<OplogEntry> oplogEntries;

    ForceSupportsDocLocking support(true);
    _testConcurrentLogOp(
        [](const NamespaceString& nss,
           stdx::mutex* mtx,
           OpTimeNamespaceStringMap* opTimeNssMap,
           unittest::Barrier* barrier) {
            return [=] {
                auto opCtx = cc().makeOperationContext();
                AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
                WriteUnitOfWork wunit(opCtx.get());

                auto opTime = _logOpNoopWithMsg(opCtx.get(), mtx, opTimeNssMap, nss);

                // In a storage engine that supports doc locking, it is okay for multiple threads to
                // maintain uncommitted WUOWs upon returning from logOp() because each thread will
                // hold an implicit MODE_IX lock on the oplog collection.
                barrier->countDownAndWait();

                // Revert the first logOp() call and confirm that there are no holes in the
                // oplog after committing the oplog entry with the more recent optime.
                {
                    stdx::lock_guard<stdx::mutex> lock(*mtx);
                    auto firstOpTimeAndNss = *(opTimeNssMap->cbegin());
                    if (opTime == firstOpTimeAndNss.first) {
                        ASSERT_EQUALS(nss, firstOpTimeAndNss.second)
                            << "optime matches entry in optime->nss map but namespace in map  is "
                               "different.";
                        // Abort WUOW by returning early. The oplog entry for this task should not
                        // be present in the oplog.
                        return;
                    }
                }

                wunit.commit();
            };
        },
        &opTimeNssMap,
        &oplogEntries,
        1U);

    _checkOplogEntry(oplogEntries[0], *(opTimeNssMap.crbegin()));
}

TEST_F(OplogTest, ConcurrentLogOpWithDocLockingSupportRevertLastOplogEntry) {
    OpTimeNamespaceStringMap opTimeNssMap;
    std::vector<OplogEntry> oplogEntries;

    ForceSupportsDocLocking support(true);
    _testConcurrentLogOp(
        [](const NamespaceString& nss,
           stdx::mutex* mtx,
           OpTimeNamespaceStringMap* opTimeNssMap,
           unittest::Barrier* barrier) {
            return [=] {
                auto opCtx = cc().makeOperationContext();
                AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
                WriteUnitOfWork wunit(opCtx.get());

                auto opTime = _logOpNoopWithMsg(opCtx.get(), mtx, opTimeNssMap, nss);

                // In a storage engine that supports doc locking, it is okay for multiple threads to
                // maintain uncommitted WUOWs upon returning from logOp() because each thread will
                // hold an implicit MODE_IX lock on the oplog collection.
                barrier->countDownAndWait();

                // Revert the last logOp() call and confirm that there are no holes in the
                // oplog after committing the oplog entry with the earlier optime.
                {
                    stdx::lock_guard<stdx::mutex> lock(*mtx);
                    auto lastOpTimeAndNss = *(opTimeNssMap->crbegin());
                    if (opTime == lastOpTimeAndNss.first) {
                        ASSERT_EQUALS(nss, lastOpTimeAndNss.second)
                            << "optime matches entry in optime->nss map but namespace in map is "
                               "different.";
                        // Abort WUOW by returning early. The oplog entry for this task should not
                        // be present in the oplog.
                        return;
                    }
                }

                wunit.commit();
            };
        },
        &opTimeNssMap,
        &oplogEntries,
        1U);

    _checkOplogEntry(oplogEntries[0], *(opTimeNssMap.cbegin()));
}

}  // namespace
}  // namespace repl
}  // namespace mongo
