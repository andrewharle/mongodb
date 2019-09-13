
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

#include <boost/optional.hpp>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/global_initializer_registerer.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/list.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/tick_source.h"

namespace mongo {

class AbstractMessagingPort;
class Client;
class OperationContext;
class OpObserver;
class ServiceEntryPoint;

namespace transport {
class TransportLayer;
}  // namespace transport

/**
 * Classes that implement this interface can receive notification on killOp.
 *
 * See registerKillOpListener() for more information,
 * including limitations on the lifetime of registered listeners.
 */
class KillOpListenerInterface {
    MONGO_DISALLOW_COPYING(KillOpListenerInterface);

public:
    /**
     * Will be called *after* ops have been told they should die.
     * Callback must not fail.
     */
    virtual void interrupt(unsigned opId) = 0;
    virtual void interruptAll() = 0;

protected:
    KillOpListenerInterface() = default;

    // Should not delete through a pointer of this type
    virtual ~KillOpListenerInterface() = default;
};

/**
 * Class representing the context of a service, such as a MongoD database service or
 * a MongoS routing service.
 *
 * A ServiceContext is the root of a hierarchy of contexts.  A ServiceContext owns
 * zero or more Clients, which in turn each own OperationContexts.
 */
class ServiceContext final : public Decorable<ServiceContext> {
    MONGO_DISALLOW_COPYING(ServiceContext);

public:
    /**
     * Observer interface implemented to hook client and operation context creation and
     * destruction.
     */
    class ClientObserver {
    public:
        virtual ~ClientObserver() = default;

        /**
         * Hook called after a new client "client" is created on a service by
         * service->makeClient().
         *
         * For a given client and registered instance of ClientObserver, if onCreateClient
         * returns without throwing an exception, onDestroyClient will be called when "client"
         * is deleted.
         */
        virtual void onCreateClient(Client* client) = 0;

        /**
         * Hook called on a "client" created by a service before deleting "client".
         *
         * Like a destructor, must not throw exceptions.
         */
        virtual void onDestroyClient(Client* client) = 0;

        /**
         * Hook called after a new operation context is created on a client by
         * service->makeOperationContext(client)  or client->makeOperationContext().
         *
         * For a given operation context and registered instance of ClientObserver, if
         * onCreateOperationContext returns without throwing an exception,
         * onDestroyOperationContext will be called when "opCtx" is deleted.
         */
        virtual void onCreateOperationContext(OperationContext* opCtx) = 0;

        /**
         * Hook called on a "opCtx" created by a service before deleting "opCtx".
         *
         * Like a destructor, must not throw exceptions.
         */
        virtual void onDestroyOperationContext(OperationContext* opCtx) = 0;
    };

    using ClientSet = stdx::unordered_set<Client*>;

    /**
     * Cursor for enumerating the live Client objects belonging to a ServiceContext.
     *
     * Lifetimes of this type are synchronized with client creation and destruction.
     */
    class LockedClientsCursor {
    public:
        /**
         * Constructs a cursor for enumerating the clients of "service", blocking "service" from
         * creating or destroying Client objects until this instance is destroyed.
         */
        explicit LockedClientsCursor(ServiceContext* service);

        /**
         * Returns the next client in the enumeration, or nullptr if there are no more clients.
         */
        Client* next();

    private:
        stdx::unique_lock<stdx::mutex> _lock;
        ClientSet::const_iterator _curr;
        ClientSet::const_iterator _end;
    };

    /**
     * Special deleter used for cleaning up ServiceContext objects.
     * See UniqueServiceContext, below.
     */
    class ServiceContextDeleter {
    public:
        void operator()(ServiceContext* service) const;
    };

    using UniqueServiceContext = std::unique_ptr<ServiceContext, ServiceContextDeleter>;

    /**
     * Special deleter used for cleaning up Client objects owned by a ServiceContext.
     * See UniqueClient, below.
     */
    class ClientDeleter {
    public:
        void operator()(Client* client) const;
    };

    /**
     * This is the unique handle type for Clients created by a ServiceContext.
     */
    using UniqueClient = std::unique_ptr<Client, ClientDeleter>;

    /**
     * Special deleter used for cleaning up OperationContext objects owned by a ServiceContext.
     * See UniqueOperationContext, below.
     */
    class OperationContextDeleter {
    public:
        void operator()(OperationContext* opCtx) const;
    };

    /**
     * This is the unique handle type for OperationContexts created by a ServiceContext.
     */
    using UniqueOperationContext = std::unique_ptr<OperationContext, OperationContextDeleter>;

    /**
     * Register a function of this type using  an instance of ConstructorActionRegisterer,
     * below, to cause the function to be executed on new ServiceContext instances.
     */
    using ConstructorAction = stdx::function<void(ServiceContext*)>;

    /**
     * Register a function of this type using an instance of ConstructorActionRegisterer,
     * below, to cause the function to be executed on ServiceContext instances before they
     * are destroyed.
     */
    using DestructorAction = stdx::function<void(ServiceContext*) noexcept>;

    /**
     * Representation of a paired ConstructorAction and DestructorAction.
     */
    class ConstructorDestructorActions {
    public:
        ConstructorDestructorActions(ConstructorAction constructor, DestructorAction destructor)
            : _constructor(std::move(constructor)), _destructor(std::move(destructor)) {}

        void onCreate(ServiceContext* service) const {
            _constructor(service);
        }
        void onDestroy(ServiceContext* service) const {
            _destructor(service);
        }

    private:
        ConstructorAction _constructor;
        DestructorAction _destructor;
    };

    /**
     * Registers a function to execute on new service contexts when they are created, and optionally
     * also register a function to execute before those contexts are destroyed.
     *
     * Construct instances of this type during static initialization only, as they register
     * MONGO_INITIALIZERS.
     */
    class ConstructorActionRegisterer {
    public:
        /**
         * This constructor registers a constructor and optional destructor with the given
         * "name" and no prerequisite constructors or mongo initializers.
         */
        ConstructorActionRegisterer(std::string name,
                                    ConstructorAction constructor,
                                    DestructorAction destructor = {});

        /**
         * This constructor registers a constructor and optional destructor with the given
         * "name", and a list of names of prerequisites, "prereqs".
         *
         * The named constructor will run after all of its prereqs successfully complete,
         * and the corresponding destructor, if provided, will run before any of its
         * prerequisites execute.
         */
        ConstructorActionRegisterer(std::string name,
                                    std::vector<std::string> prereqs,
                                    ConstructorAction constructor,
                                    DestructorAction destructor = {});

    private:
        using ConstructorActionListIterator = stdx::list<ConstructorDestructorActions>::iterator;
        ConstructorActionListIterator _iter;
        boost::optional<GlobalInitializerRegisterer> _registerer;
    };

    /**
     * Factory function for making instances of ServiceContext. It is the only means by which they
     * should be created.
     */
    static UniqueServiceContext make();

    ServiceContext();
    ~ServiceContext();

    /**
     * Registers an observer of lifecycle events on Clients created by this ServiceContext.
     *
     * See the ClientObserver type, above, for details.
     *
     * All calls to registerClientObserver must complete before ServiceContext
     * is used in multi-threaded operation, or is used to create clients via calls
     * to makeClient.
     */
    void registerClientObserver(std::unique_ptr<ClientObserver> observer);

    /**
     * Creates a new Client object representing a client session associated with this
     * ServiceContext.
     *
     * The "desc" string is used to set a descriptive name for the client, used in logging.
     *
     * If supplied, "session" is the transport::Session used for communicating with the client.
     */
    UniqueClient makeClient(std::string desc, transport::SessionHandle session = nullptr);

    /**
     * Creates a new OperationContext on "client".
     *
     * "client" must not have an active operation context.
     *
     */
    UniqueOperationContext makeOperationContext(Client* client);

    //
    // Storage
    //

    /**
     * Sets the storage engine for this instance. May be called up to once per instance.
     */
    void setStorageEngine(std::unique_ptr<StorageEngine> engine);

    /**
     * Return the storage engine instance we're using.
     */
    StorageEngine* getStorageEngine() {
        return _storageEngine.get();
    }

    //
    // Global operation management.  This may not belong here and there may be too many methods
    // here.
    //

    /**
     * Signal all OperationContext(s) that they have been killed.
     */
    void setKillAllOperations();

    /**
     * Reset the operation kill state after a killAllOperations.
     * Used for testing.
     */
    void unsetKillAllOperations();

    /**
     * Get the state for killing all operations.
     */
    bool getKillAllOperations() {
        return _globalKill.loadRelaxed();
    }

    /**
     * Kills the operation "opCtx" with the code "killCode", if opCtx has not already been killed.
     * Caller must own the lock on opCtx->getClient, and opCtx->getServiceContext() must be the same
     *as
     * this service context.
     **/
    void killOperation(OperationContext* opCtx,
                       ErrorCodes::Error killCode = ErrorCodes::Interrupted);

    /**
     * Kills all operations that have a Client that is associated with an incoming user
     * connection, except for the one associated with opCtx.
     */
    void killAllUserOperations(const OperationContext* opCtx, ErrorCodes::Error killCode);

    /**
     * Registers a listener to be notified each time an op is killed.
     *
     * listener does not become owned by the environment. As there is currently no way to
     * unregister, the listener object must outlive this ServiceContext object.
     */
    void registerKillOpListener(KillOpListenerInterface* listener);

    //
    // Background tasks.
    //

    /**
     * Set a periodic runner on the service context. The runner should already be
     * started when it is moved onto the service context. The service context merely
     * takes ownership of this object to allow it to continue running for the life of
     * the process
     */
    void setPeriodicRunner(std::unique_ptr<PeriodicRunner> runner);

    /**
     * Returns a pointer to the global periodic runner owned by this service context.
     */
    PeriodicRunner* getPeriodicRunner() const;

    //
    // Transport.
    //

    /**
     * Get the master TransportLayer. Routes to all other TransportLayers that
     * may be in use within this service.
     *
     * See TransportLayerManager for more details.
     */
    transport::TransportLayer* getTransportLayer() const;

    /**
     * Get the service entry point for the service context.
     *
     * See ServiceEntryPoint for more details.
     */
    ServiceEntryPoint* getServiceEntryPoint() const;

    /**
     * Get the service executor for the service context.
     *
     * See ServiceStateMachine for how this is used. Some configurations may not have a service
     * executor registered and this will return a nullptr.
     */
    transport::ServiceExecutor* getServiceExecutor() const;

    /**
     * Waits for the ServiceContext to be fully initialized and for all TransportLayers to have been
     * added/started.
     *
     * If startup is already complete this returns immediately.
     */
    void waitForStartupComplete();

    /*
     * Marks initialization as complete and all transport layers as started.
     */
    void notifyStartupComplete();

    /*
     * Returns the number of active client operations
     */
    int getActiveClientOperations();

    /**
     * Set the OpObserver.
     */
    void setOpObserver(std::unique_ptr<OpObserver> opObserver);

    /**
     * Return the OpObserver instance we're using. This may be an OpObserverRegistry that in fact
     * contains multiple observers.
     */
    OpObserver* getOpObserver() const {
        return _opObserver.get();
    }

    /**
     * Returns the tick/clock source set in this context.
     */
    TickSource* getTickSource() const {
        return _tickSource.get();
    }

    /**
     * Get a ClockSource implementation that may be less precise than the _preciseClockSource but
     * may be cheaper to call.
     */
    ClockSource* getFastClockSource() const {
        return _fastClockSource.get();
    }

    /**
     * Get a ClockSource implementation that is very precise but may be expensive to call.
     */
    ClockSource* getPreciseClockSource() const {
        return _preciseClockSource.get();
    }

    /**
     * Replaces the current tick/clock source with a new one. In other words, the old source will be
     * destroyed. So make sure that no one is using the old source when calling this.
     */
    void setTickSource(std::unique_ptr<TickSource> newSource);

    /**
     * Call this method with a ClockSource implementation that may be less precise than
     * the _preciseClockSource but may be cheaper to call.
     */
    void setFastClockSource(std::unique_ptr<ClockSource> newSource);

    /**
     * Call this method with a ClockSource implementation that is very precise but
     * may be expensive to call.
     */
    void setPreciseClockSource(std::unique_ptr<ClockSource> newSource);

    /**
     * Binds the service entry point implementation to the service context.
     */
    void setServiceEntryPoint(std::unique_ptr<ServiceEntryPoint> sep);

    /**
     * Binds the TransportLayer to the service context. The TransportLayer should have already
     * had setup() called successfully, but not startup().
     *
     * This should be a TransportLayerManager created with the global server configuration.
     */
    void setTransportLayer(std::unique_ptr<transport::TransportLayer> tl);

    /**
     * Binds the service executor to the service context
     */
    void setServiceExecutor(std::unique_ptr<transport::ServiceExecutor> exec);

private:
    class ClientObserverHolder {
    public:
        explicit ClientObserverHolder(std::unique_ptr<ClientObserver> observer)
            : _observer(std::move(observer)) {}
        void onCreate(Client* client) const {
            _observer->onCreateClient(client);
        }
        void onDestroy(Client* client) const {
            _observer->onDestroyClient(client);
        }
        void onCreate(OperationContext* opCtx) const {
            _observer->onCreateOperationContext(opCtx);
        }
        void onDestroy(OperationContext* opCtx) const {
            _observer->onDestroyOperationContext(opCtx);
        }

    private:
        std::unique_ptr<ClientObserver> _observer;
    };

    stdx::mutex _mutex;

    /**
     * The periodic runner.
     */
    std::unique_ptr<PeriodicRunner> _runner;

    /**
     * The TransportLayer.
     */
    std::unique_ptr<transport::TransportLayer> _transportLayer;

    /**
     * The service entry point
     */
    std::unique_ptr<ServiceEntryPoint> _serviceEntryPoint;

    /**
     * The ServiceExecutor
     */
    std::unique_ptr<transport::ServiceExecutor> _serviceExecutor;

    /**
     * The storage engine, if any.
     */
    std::unique_ptr<StorageEngine> _storageEngine;

    /**
     * Vector of registered observers.
     */
    std::vector<ClientObserverHolder> _clientObservers;
    ClientSet _clients;

    /**
     * The registered OpObserver.
     */
    std::unique_ptr<OpObserver> _opObserver;

    std::unique_ptr<TickSource> _tickSource;

    /**
     * A ClockSource implementation that may be less precise than the _preciseClockSource but
     * may be cheaper to call.
     */
    std::unique_ptr<ClockSource> _fastClockSource;

    /**
     * A ClockSource implementation that is very precise but may be expensive to call.
     */
    std::unique_ptr<ClockSource> _preciseClockSource;

    // Flag set to indicate that all operations are to be interrupted ASAP.
    AtomicWord<bool> _globalKill{false};

    // protected by _mutex
    std::vector<KillOpListenerInterface*> _killOpListeners;

    // Counter for assigning operation ids.
    AtomicUInt32 _nextOpId{1};

    bool _startupComplete = false;
    stdx::condition_variable _startupCompleteCondVar;
};

/**
 * Returns true if there is a global ServiceContext.
 */
bool hasGlobalServiceContext();

/**
 * Returns the singleton ServiceContext for this server process.
 *
 * Fatal if there is currently no global ServiceContext.
 *
 * Caller does not own pointer.
 */
ServiceContext* getGlobalServiceContext();

/**
 * Sets the global ServiceContext.  If 'serviceContext' is NULL, un-sets and deletes
 * the current global ServiceContext.
 *
 * Takes ownership of 'serviceContext'.
 */
void setGlobalServiceContext(ServiceContext::UniqueServiceContext&& serviceContext);

/**
 * Shortcut for querying the storage engine about whether it supports document-level locking.
 * If this call becomes too expensive, we could cache the value somewhere so we don't have to
 * fetch the storage engine every time.
 */
bool supportsDocLocking();

/**
 * Returns true if the storage engine in use is MMAPV1.
 */
bool isMMAPV1();

}  // namespace mongo
