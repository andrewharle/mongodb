
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

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/s/namespace_metadata_change_notifications.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

const NamespaceString kNss("foo.bar");

class NamespaceMetadataChangeNotificationsTest : public ServiceContextMongoDTest {
protected:
    NamespaceMetadataChangeNotificationsTest() {
        getServiceContext()->setTickSource(stdx::make_unique<TickSourceMock>());
    }
};

TEST_F(NamespaceMetadataChangeNotificationsTest, WaitForNotify) {
    NamespaceMetadataChangeNotifications notifications;

    auto scopedNotif = notifications.createNotification(kNss);

    {
        auto opCtx = getClient()->makeOperationContext();
        opCtx->setDeadlineAfterNowBy(Milliseconds{0}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(
            scopedNotif.get(opCtx.get()), AssertionException, ErrorCodes::ExceededTimeLimit);
    }

    notifications.notifyChange(kNss);

    {
        auto opCtx = getClient()->makeOperationContext();
        scopedNotif.get(opCtx.get());
    }
}

TEST_F(NamespaceMetadataChangeNotificationsTest, GiveUpWaitingForNotify) {
    NamespaceMetadataChangeNotifications notifications;

    {
        auto scopedNotif = notifications.createNotification(kNss);
        auto opCtx = getClient()->makeOperationContext();
        opCtx->setDeadlineAfterNowBy(Milliseconds{0}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(
            scopedNotif.get(opCtx.get()), AssertionException, ErrorCodes::ExceededTimeLimit);
    }

    notifications.notifyChange(kNss);
}

TEST_F(NamespaceMetadataChangeNotificationsTest, MoveConstructionWaitForNotify) {
    NamespaceMetadataChangeNotifications notifications;

    auto scopedNotif = notifications.createNotification(kNss);
    auto movedScopedNotif = std::move(scopedNotif);

    {
        auto opCtx = getClient()->makeOperationContext();
        opCtx->setDeadlineAfterNowBy(Milliseconds{0}, ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(
            movedScopedNotif.get(opCtx.get()), AssertionException, ErrorCodes::ExceededTimeLimit);
    }

    notifications.notifyChange(kNss);

    {
        auto opCtx = getClient()->makeOperationContext();
        movedScopedNotif.get(opCtx.get());
    }
}

}  // namespace
}  // namespace mongo
