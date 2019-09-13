
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

#pragma once

#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/db/key_generator.h"
#include "mongo/db/keys_collection_cache.h"
#include "mongo/db/keys_collection_document.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"

namespace mongo {

class OperationContext;
class LogicalTime;
class ServiceContext;
class KeysCollectionClient;

extern int KeysRotationIntervalSec;

/**
 * The KeysCollectionManager queries the config servers for keys that can be used for
 * HMAC computation. It maintains an internal background thread that is used to periodically
 * refresh the local key cache against the keys collection stored on the config servers.
 */
class KeysCollectionManager {
public:
    static const Seconds kKeyValidInterval;
    static const std::string kKeyManagerPurposeString;

    KeysCollectionManager(std::string purpose,
                          std::unique_ptr<KeysCollectionClient> client,
                          Seconds keyValidForInterval);

    /**
     * Return a key that is valid for the given time and also matches the keyId. Note that this call
     * can block if it will need to do a refresh.
     *
     * Throws ErrorCode::ExceededTimeLimit if it times out.
     */
    StatusWith<KeysCollectionDocument> getKeyForValidation(OperationContext* opCtx,
                                                           long long keyId,
                                                           const LogicalTime& forThisTime);

    /**
     * Returns a key that is valid for the given time. Note that unlike getKeyForValidation, this
     * will never do a refresh.
     *
     * Throws ErrorCode::ExceededTimeLimit if it times out.
     */
    StatusWith<KeysCollectionDocument> getKeyForSigning(OperationContext* opCtx,
                                                        const LogicalTime& forThisTime);

    /**
     * Request this manager to perform a refresh.
     */
    void refreshNow(OperationContext* opCtx);

    /**
     * Starts a background thread that will constantly update the internal cache of keys.
     *
     * Cannot call this after stopMonitoring was called at least once.
     */
    void startMonitoring(ServiceContext* service);

    /**
     * Stops the background thread updating the cache.
     */
    void stopMonitoring();

    /**
     * Enable writing new keys to the config server primary. Should only be called if current node
     * is the config primary.
     */
    void enableKeyGenerator(OperationContext* opCtx, bool doEnable);

    /**
     * Returns true if the refresher has ever successfully returned keys from the config server.
     */
    bool hasSeenKeys();

    /**
     * Clears the in memory cache of the keys.
     */
    void clearCache();

private:
    /**
     * This is responsible for periodically performing refresh in the background.
     */
    class PeriodicRunner {
    public:
        using RefreshFunc = stdx::function<StatusWith<KeysCollectionDocument>(OperationContext*)>;

        /**
         * Preemptively inform the monitoring thread it needs to perform a refresh. Returns an
         * object
         * that gets notified after the current round of refresh is over. Note that being notified
         * can
         * mean either of these things:
         *
         * 1. An error occurred and refresh was not performed.
         * 2. No error occurred but no new key was found.
         * 3. No error occurred and new keys were found.
         */
        void refreshNow(OperationContext* opCtx);

        /**
         * Sets the refresh function to use.
         * Should only be used to bootstrap this refresher with initial strategy.
         */
        void setFunc(RefreshFunc newRefreshStrategy);

        /**
         * Switches the current strategy with a new one. This also waits to make sure that the old
         * strategy is not being used and will no longer be used after this call.
         */
        void switchFunc(OperationContext* opCtx, RefreshFunc newRefreshStrategy);

        /**
         * Starts the refresh thread.
         */
        void start(ServiceContext* service,
                   const std::string& threadName,
                   Milliseconds refreshInterval);

        /**
         * Stops the refresh thread.
         */
        void stop();

        /**
         * Returns true if keys have ever successfully been returned from the config server.
         */
        bool hasSeenKeys();

    private:
        void _doPeriodicRefresh(ServiceContext* service,
                                std::string threadName,
                                Milliseconds refreshInterval);

        stdx::mutex _mutex;  // protects all the member variables below.
        std::shared_ptr<Notification<void>> _refreshRequest;
        stdx::condition_variable _refreshNeededCV;

        stdx::thread _backgroundThread;
        std::shared_ptr<RefreshFunc> _doRefresh;

        bool _hasSeenKeys = false;
        bool _inShutdown = false;
    };

    /**
     * Return a key that is valid for the given time and also matches the keyId.
     */
    StatusWith<KeysCollectionDocument> _getKeyWithKeyIdCheck(long long keyId,
                                                             const LogicalTime& forThisTime);

    /**
     * Return a key that is valid for the given time.
     */
    StatusWith<KeysCollectionDocument> _getKey(const LogicalTime& forThisTime);

    std::unique_ptr<KeysCollectionClient> _client;
    const std::string _purpose;
    const Seconds _keyValidForInterval;

    // No mutex needed since the members below have their own mutexes.
    KeysCollectionCache _keysCache;
    PeriodicRunner _refresher;
};

}  // namespace mongo
