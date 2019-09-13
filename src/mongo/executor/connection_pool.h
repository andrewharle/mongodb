
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
#include <queue>

#include "mongo/base/disallow_copying.h"
#include "mongo/executor/egress_tag_closer.h"
#include "mongo/executor/egress_tag_closer_manager.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObjBuilder;

namespace executor {

struct ConnectionPoolStats;

/**
 * The actual user visible connection pool.
 *
 * This pool is constructed with a DependentTypeFactoryInterface which provides the tools it
 * needs to generate connections and manage them over time.
 *
 * The overall workflow here is to manage separate pools for each unique
 * HostAndPort. See comments on the various Options for how the pool operates.
 */
class ConnectionPool : public EgressTagCloser {
    class SpecificPool;

public:
    class ConnectionInterface;
    class DependentTypeFactoryInterface;
    class TimerInterface;

    using ConnectionHandleDeleter = stdx::function<void(ConnectionInterface* connection)>;
    using ConnectionHandle = std::unique_ptr<ConnectionInterface, ConnectionHandleDeleter>;

    using GetConnectionCallback = stdx::function<void(StatusWith<ConnectionHandle>)>;

    static constexpr Milliseconds kDefaultHostTimeout = Milliseconds(300000);  // 5mins
    static const size_t kDefaultMaxConns;
    static const size_t kDefaultMinConns;
    static const size_t kDefaultMaxConnecting;
    static constexpr Milliseconds kDefaultRefreshRequirement = Milliseconds(60000);  // 1min
    static constexpr Milliseconds kDefaultRefreshTimeout = Milliseconds(20000);      // 20secs

    static const Status kConnectionStateUnknown;

    struct Options {
        Options() {}

        /**
         * The minimum number of connections to keep alive while the pool is in
         * operation
         */
        size_t minConnections = kDefaultMinConns;

        /**
         * The maximum number of connections to spawn for a host. This includes
         * pending connections in setup and connections checked out of the pool
         * as well as the obvious live connections in the pool.
         */
        size_t maxConnections = kDefaultMaxConns;

        /**
         * The maximum number of processing connections for a host.  This includes pending
         * connections in setup/refresh. It's designed to rate limit connection storms rather than
         * steady state processing (as maxConnections does).
         */
        size_t maxConnecting = kDefaultMaxConnecting;

        /**
         * Amount of time to wait before timing out a refresh attempt
         */
        Milliseconds refreshTimeout = kDefaultRefreshTimeout;

        /**
         * Amount of time a connection may be idle before it cannot be returned
         * for a user request and must instead be checked out and refreshed
         * before handing to a user.
         */
        Milliseconds refreshRequirement = kDefaultRefreshRequirement;

        /**
         * Amount of time to keep a specific pool around without any checked
         * out connections or new requests
         */
        Milliseconds hostTimeout = kDefaultHostTimeout;

        /**
         * An egress tag closer manager which will provide global access to this connection pool.
         * The manager set's tags and potentially drops connections that don't match those tags.
         *
         * The manager will hold this pool for the lifetime of the pool.
         */
        EgressTagCloserManager* egressTagCloserManager = nullptr;
    };

    explicit ConnectionPool(std::shared_ptr<DependentTypeFactoryInterface> impl,
                            std::string name,
                            Options options = Options{});

    ~ConnectionPool();

    void shutdown();

    void dropConnections(const HostAndPort& hostAndPort);

    void dropConnections(transport::Session::TagMask tags) override;

    void mutateTags(const HostAndPort& hostAndPort,
                    const stdx::function<transport::Session::TagMask(transport::Session::TagMask)>&
                        mutateFunc) override;

    Future<ConnectionHandle> get(const HostAndPort& hostAndPort,
                                 transport::ConnectSSLMode sslMode,
                                 Milliseconds timeout);
    void get_forTest(const HostAndPort& hostAndPort,
                     Milliseconds timeout,
                     GetConnectionCallback cb);

    void appendConnectionStats(ConnectionPoolStats* stats) const;

    size_t getNumConnectionsPerHost(const HostAndPort& hostAndPort) const;

private:
    void returnConnection(ConnectionInterface* connection);

    std::string _name;

    // Options are set at startup and never changed at run time, so these are
    // accessed outside the lock
    const Options _options;

    const std::shared_ptr<DependentTypeFactoryInterface> _factory;

    // The global mutex for specific pool access and the generation counter
    mutable stdx::mutex _mutex;
    stdx::unordered_map<HostAndPort, std::shared_ptr<SpecificPool>> _pools;

    EgressTagCloserManager* _manager;
};

/**
 * Interface for a basic timer
 *
 * Minimal interface sets a timer with a callback and cancels the timer.
 */
class ConnectionPool::TimerInterface {
    MONGO_DISALLOW_COPYING(TimerInterface);

public:
    TimerInterface() = default;

    using TimeoutCallback = stdx::function<void()>;

    virtual ~TimerInterface() = default;

    /**
     * Sets the timeout for the timer. Setting an already set timer should
     * override the previous timer.
     */
    virtual void setTimeout(Milliseconds timeout, TimeoutCallback cb) = 0;

    /**
     * It should be safe to cancel a previously canceled, or never set, timer.
     */
    virtual void cancelTimeout() = 0;

    /**
     * Returns the current time for the clock used by the timer
     */
    virtual Date_t now() = 0;
};

/**
 * Interface for connection pool connections
 *
 * Provides a minimal interface to manipulate connections within the pool,
 * specifically callbacks to set them up (connect + auth + whatever else),
 * refresh them (issue some kind of ping) and manage a timer.
 */
class ConnectionPool::ConnectionInterface : public TimerInterface {
    MONGO_DISALLOW_COPYING(ConnectionInterface);

    friend class ConnectionPool;

public:
    explicit ConnectionInterface(size_t generation) : _generation(generation) {}

    virtual ~ConnectionInterface() = default;

    /**
     * Indicates that the user is now done with this connection. Users MUST call either
     * this method or indicateFailure() before returning the connection to its pool.
     */
    void indicateSuccess();

    /**
     * Indicates that a connection has failed. This will prevent the connection
     * from re-entering the connection pool. Users MUST call either this method or
     * indicateSuccess() before returning connections to the pool.
     */
    void indicateFailure(Status status);

    /**
     * This method updates a 'liveness' timestamp to avoid unnecessarily refreshing
     * the connection.
     *
     * This method should be invoked whenever we perform an operation on the connection that must
     * have done work.  I.e. actual networking was performed.  If a connection was checked out, then
     * back in without use, one would expect an indicateSuccess without an indicateUsed.  Only if we
     * checked it out and did work would we call indicateUsed.
     */
    void indicateUsed();

    /**
     * The HostAndPort for the connection. This should be the same as the
     * HostAndPort passed to DependentTypeFactoryInterface::makeConnection.
     */
    virtual const HostAndPort& getHostAndPort() const = 0;
    virtual transport::ConnectSSLMode getSslMode() const = 0;

    /**
     * Check if the connection is healthy using some implementation defined condition.
     */
    virtual bool isHealthy() = 0;

    /**
     * Returns the last used time point for the connection
     */
    Date_t getLastUsed() const;

    /**
     * Returns the status associated with the connection. If the status is not
     * OK, the connection will not be returned to the pool.
     */
    const Status& getStatus() const;

    /**
     * Get the generation of the connection. This is used to track whether to
     * continue using a connection after a call to dropConnections() by noting
     * if the generation on the specific pool is the same as the generation on
     * a connection (if not the connection is from a previous era and should
     * not be re-used).
     */
    size_t getGeneration() const;

protected:
    /**
     * Making these protected makes the definitions available to override in
     * children.
     */
    using SetupCallback = stdx::function<void(ConnectionInterface*, Status)>;
    using RefreshCallback = stdx::function<void(ConnectionInterface*, Status)>;

    /**
     * Sets up the connection. This should include connection + auth + any
     * other associated hooks.
     */
    virtual void setup(Milliseconds timeout, SetupCallback cb) = 0;

    /**
     * Resets the connection's state to kConnectionStateUnknown for the next user.
     */
    void resetToUnknown();

    /**
     * Refreshes the connection. This should involve a network round trip and
     * should strongly imply an active connection
     */
    virtual void refresh(Milliseconds timeout, RefreshCallback cb) = 0;

private:
    size_t _generation;
    Date_t _lastUsed;
    Status _status = ConnectionPool::kConnectionStateUnknown;
};

/**
 * Implementation interface for the connection pool
 *
 * This factory provides generators for connections, timers and a clock for the
 * connection pool.
 */
class ConnectionPool::DependentTypeFactoryInterface {
    MONGO_DISALLOW_COPYING(DependentTypeFactoryInterface);

public:
    DependentTypeFactoryInterface() = default;

    virtual ~DependentTypeFactoryInterface() = default;

    /**
     * Makes a new connection given a host and port
     */
    virtual std::shared_ptr<ConnectionInterface> makeConnection(const HostAndPort& hostAndPort,
                                                                transport::ConnectSSLMode sslMode,
                                                                size_t generation) = 0;

    /**
     * Makes a new timer
     */
    virtual std::shared_ptr<TimerInterface> makeTimer() = 0;

    /**
     * Returns the current time point
     */
    virtual Date_t now() = 0;

    /**
     * shutdown
     */
    virtual void shutdown() = 0;
};

}  // namespace executor
}  // namespace mongo
