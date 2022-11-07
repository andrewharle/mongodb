
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/transport_layer_asio.h"

#include <asio.hpp>
#include <asio/system_timer.hpp>
#include <boost/algorithm/string.hpp>

#include "mongo/config.h"

#include "mongo/base/system_error.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/asio_utils.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl.hpp"
#endif

// session_asio.h has some header dependencies that require it to be the last header.
#ifdef __linux__
#include "mongo/transport/baton_asio_linux.h"
#endif
#include "mongo/transport/session_asio.h"

namespace mongo {
namespace transport {

MONGO_FAIL_POINT_DEFINE(transportLayerASIOasyncConnectTimesOut);

class ASIOReactorTimer final : public ReactorTimer {
public:
    explicit ASIOReactorTimer(asio::io_context& ctx)
        : _timer(std::make_shared<asio::system_timer>(ctx)) {}

    ~ASIOReactorTimer() {
        // The underlying timer won't get destroyed until the last promise from _asyncWait
        // has been filled, so cancel the timer so our promises get fulfilled
        cancel();
    }

    void cancel(const BatonHandle& baton = nullptr) override {
        // If we have a baton try to cancel that.
        if (baton && baton->cancelTimer(*this)) {
            LOG(2) << "Canceled via baton, skipping asio cancel.";
            return;
        }

        // Otherwise there could be a previous timer that was scheduled normally.
        _timer->cancel();
    }

    Future<void> waitFor(Milliseconds timeout, const BatonHandle& baton = nullptr) override {
        if (baton) {
            return _asyncWait([&] { return baton->waitFor(*this, timeout); }, baton);
        } else {
            return _asyncWait([&] { _timer->expires_after(timeout.toSystemDuration()); });
        }
    }

    Future<void> waitUntil(Date_t expiration, const BatonHandle& baton = nullptr) override {
        if (baton) {
            return _asyncWait([&] { return baton->waitUntil(*this, expiration); }, baton);
        } else {
            return _asyncWait([&] { _timer->expires_at(expiration.toSystemTimePoint()); });
        }
    }

private:
    template <typename ArmTimerCb>
    Future<void> _asyncWait(ArmTimerCb&& armTimer) {
        try {
            cancel();

            armTimer();
            return _timer->async_wait(UseFuture{}).tapError([timer = _timer](const Status& status) {
                LOG(2) << "Timer received error: " << status;
            });

        } catch (asio::system_error& ex) {
            return futurize(ex.code());
        }
    }

    template <typename ArmTimerCb>
    Future<void> _asyncWait(ArmTimerCb&& armTimer, const BatonHandle& baton) {
        cancel(baton);

        auto pf = makePromiseFuture<void>();
        armTimer().getAsync([sp = pf.promise.share()](Status status) mutable {
            if (status.isOK()) {
                sp.emplaceValue();
            } else {
                sp.setError(status);
            }
        });

        return std::move(pf.future);
    }

    std::shared_ptr<asio::system_timer> _timer;
};

class TransportLayerASIO::ASIOReactor final : public Reactor {
public:
    ASIOReactor() : _ioContext() {}

    void run() noexcept override {
        ThreadIdGuard threadIdGuard(this);
        asio::io_context::work work(_ioContext);
        try {
            _ioContext.run();
        } catch (...) {
            severe() << "Uncaught exception in reactor: " << exceptionToStatus();
            fassertFailed(40491);
        }
    }

    void runFor(Milliseconds time) noexcept override {
        ThreadIdGuard threadIdGuard(this);
        asio::io_context::work work(_ioContext);
        try {
            _ioContext.run_for(time.toSystemDuration());
        } catch (...) {
            severe() << "Uncaught exception in reactor: " << exceptionToStatus();
            fassertFailed(50473);
        }
    }

    void stop() override {
        _ioContext.stop();
    }

    void drain() override {
        _ioContext.restart();
        while (_ioContext.poll()) {
            LOG(2) << "Draining remaining work in reactor.";
        }
        _ioContext.stop();
    }

    std::unique_ptr<ReactorTimer> makeTimer() override {
        return std::make_unique<ASIOReactorTimer>(_ioContext);
    }

    Date_t now() override {
        return Date_t(asio::system_timer::clock_type::now());
    }

    void schedule(ScheduleMode mode, Task task) override {
        if (mode == kDispatch) {
            _ioContext.dispatch(std::move(task));
        } else {
            _ioContext.post(std::move(task));
        }
    }

    bool onReactorThread() const override {
        return this == _reactorForThread;
    }

    operator asio::io_context&() {
        return _ioContext;
    }

private:
    class ThreadIdGuard {
    public:
        ThreadIdGuard(TransportLayerASIO::ASIOReactor* reactor) {
            _reactorForThread = reactor;
        }

        ~ThreadIdGuard() {
            _reactorForThread = nullptr;
        }
    };

    static thread_local ASIOReactor* _reactorForThread;

    asio::io_context _ioContext;
};

thread_local TransportLayerASIO::ASIOReactor* TransportLayerASIO::ASIOReactor::_reactorForThread =
    nullptr;

TransportLayerASIO::Options::Options(const ServerGlobalParams* params)
    : port(params->port),
      ipList(params->bind_ips),
#ifndef _WIN32
      useUnixSockets(!params->noUnixSocket),
#endif
      enableIPv6(params->enableIPv6),
      maxConns(params->maxConns) {
}

TransportLayerASIO::TransportLayerASIO(const TransportLayerASIO::Options& opts,
                                       ServiceEntryPoint* sep)
    : _ingressReactor(std::make_shared<ASIOReactor>()),
      _egressReactor(std::make_shared<ASIOReactor>()),
      _acceptorReactor(std::make_shared<ASIOReactor>()),
#ifdef MONGO_CONFIG_SSL
      _ingressSSLContext(nullptr),
      _egressSSLContext(nullptr),
#endif
      _sep(sep),
      _listenerOptions(opts) {
}

TransportLayerASIO::~TransportLayerASIO() = default;

class WrappedEndpoint {
public:
    using Endpoint = asio::generic::stream_protocol::endpoint;

    explicit WrappedEndpoint(const asio::ip::basic_resolver_entry<asio::ip::tcp>& source)
        : _str(str::stream() << source.endpoint().address().to_string() << ":"
                             << source.service_name()),
          _endpoint(source.endpoint()) {}

#ifndef _WIN32
    explicit WrappedEndpoint(const asio::local::stream_protocol::endpoint& source)
        : _str(source.path()), _endpoint(source) {}
#endif

    WrappedEndpoint() = default;

    Endpoint* operator->() noexcept {
        return &_endpoint;
    }

    const Endpoint* operator->() const noexcept {
        return &_endpoint;
    }

    Endpoint& operator*() noexcept {
        return _endpoint;
    }

    const Endpoint& operator*() const noexcept {
        return _endpoint;
    }

    bool operator<(const WrappedEndpoint& rhs) const noexcept {
        return _endpoint < rhs._endpoint;
    }

    const std::string& toString() const {
        return _str;
    }

    sa_family_t family() const {
        return _endpoint.data()->sa_family;
    }

private:
    std::string _str;
    Endpoint _endpoint;
};

using Resolver = asio::ip::tcp::resolver;
class WrappedResolver {
public:
    using Flags = Resolver::flags;
    using EndpointVector = std::vector<WrappedEndpoint>;

    explicit WrappedResolver(asio::io_context& ioCtx) : _resolver(ioCtx) {}

    StatusWith<EndpointVector> resolve(const HostAndPort& peer, bool enableIPv6) {
        if (auto unixEp = _checkForUnixSocket(peer)) {
            return *unixEp;
        }

        // We always want to resolve the "service" (port number) as a numeric.
        //
        // We intentionally don't set the Resolver::address_configured flag because it might prevent
        // us from connecting to localhost on hosts with only a loopback interface
        // (see SERVER-1579).
        const auto flags = Resolver::numeric_service;

        // We resolve in two steps, the first step tries to resolve the hostname as an IP address -
        // that way if there's a DNS timeout, we can still connect to IP addresses quickly.
        // (See SERVER-1709)
        //
        // Then, if the numeric (IP address) lookup failed, we fall back to DNS or return the error
        // from the resolver.
        return _resolve(peer, flags | Resolver::numeric_host, enableIPv6)
            .onError([=](Status) { return _resolve(peer, flags, enableIPv6); })
            .getNoThrow();
    }

    Future<EndpointVector> asyncResolve(const HostAndPort& peer, bool enableIPv6) {
        if (auto unixEp = _checkForUnixSocket(peer)) {
            return *unixEp;
        }

        // We follow the same numeric -> hostname fallback procedure as the synchronous resolver
        // function for setting resolver flags (see above).
        const auto flags = Resolver::numeric_service;
        return _asyncResolve(peer, flags | Resolver::numeric_host, enableIPv6).onError([=](Status) {
            return _asyncResolve(peer, flags, enableIPv6);
        });
    }

    void cancel() {
        _resolver.cancel();
    }

private:
    boost::optional<EndpointVector> _checkForUnixSocket(const HostAndPort& peer) {
#ifndef _WIN32
        if (mongoutils::str::contains(peer.host(), '/')) {
            asio::local::stream_protocol::endpoint ep(peer.host());
            return EndpointVector{WrappedEndpoint(ep)};
        }
#endif
        return boost::none;
    }

    Future<EndpointVector> _resolve(const HostAndPort& peer, Flags flags, bool enableIPv6) {
        std::error_code ec;
        auto port = std::to_string(peer.port());
        Results results;
        if (enableIPv6) {
            results = _resolver.resolve(peer.host(), port, flags, ec);
        } else {
            results = _resolver.resolve(asio::ip::tcp::v4(), peer.host(), port, flags, ec);
        }

        if (ec) {
            return _makeFuture(errorCodeToStatus(ec), peer);
        } else {
            return _makeFuture(results, peer);
        }
    }

    Future<EndpointVector> _asyncResolve(const HostAndPort& peer, Flags flags, bool enableIPv6) {
        auto port = std::to_string(peer.port());
        Future<Results> ret;
        if (enableIPv6) {
            ret = _resolver.async_resolve(peer.host(), port, flags, UseFuture{});
        } else {
            ret =
                _resolver.async_resolve(asio::ip::tcp::v4(), peer.host(), port, flags, UseFuture{});
        }

        return std::move(ret)
            .onError([this, peer](Status status) { return _checkResults(status, peer); })
            .then([this, peer](Results results) { return _makeFuture(results, peer); });
    }

    using Results = Resolver::results_type;
    StatusWith<Results> _checkResults(StatusWith<Results> results, const HostAndPort& peer) {
        if (!results.isOK()) {
            return Status{ErrorCodes::HostNotFound,
                          str::stream() << "Could not find address for " << peer << ": "
                                        << results.getStatus()};
        } else if (results.getValue().empty()) {
            return Status{ErrorCodes::HostNotFound,
                          str::stream() << "Could not find address for " << peer};
        } else {
            return results;
        }
    }

    Future<EndpointVector> _makeFuture(StatusWith<Results> results, const HostAndPort& peer) {
        results = _checkResults(std::move(results), peer);
        if (!results.isOK()) {
            return results.getStatus();
        } else {
            auto& epl = results.getValue();
            return EndpointVector(epl.begin(), epl.end());
        }
    }

    Resolver _resolver;
};

Status makeConnectError(Status status, const HostAndPort& peer, const WrappedEndpoint& endpoint) {
    std::string errmsg;
    if (peer.toString() != endpoint.toString() && !endpoint.toString().empty()) {
        errmsg = str::stream() << "Error connecting to " << peer << " (" << endpoint.toString()
                               << ")";
    } else {
        errmsg = str::stream() << "Error connecting to " << peer;
    }

    return status.withContext(errmsg);
}


StatusWith<SessionHandle> TransportLayerASIO::connect(HostAndPort peer,
                                                      ConnectSSLMode sslMode,
                                                      Milliseconds timeout) {
    std::error_code ec;
    GenericSocket sock(*_egressReactor);
    WrappedResolver resolver(*_egressReactor);

    auto swEndpoints = resolver.resolve(peer, _listenerOptions.enableIPv6);
    if (!swEndpoints.isOK()) {
        return swEndpoints.getStatus();
    }

    auto endpoints = std::move(swEndpoints.getValue());
    auto sws = _doSyncConnect(endpoints.front(), peer, timeout);
    if (!sws.isOK()) {
        return sws.getStatus();
    }

    auto session = std::move(sws.getValue());
    session->ensureSync();

#ifndef _WIN32
    if (endpoints.front().family() == AF_UNIX) {
        return static_cast<SessionHandle>(std::move(session));
    }
#endif

#ifndef MONGO_CONFIG_SSL
    if (sslMode == kEnableSSL) {
        return {ErrorCodes::InvalidSSLConfiguration, "SSL requested but not supported"};
    }
#else
    auto globalSSLMode = _sslMode();
    if (sslMode == kEnableSSL ||
        (sslMode == kGlobalSSLMode && ((globalSSLMode == SSLParams::SSLMode_preferSSL) ||
                                       (globalSSLMode == SSLParams::SSLMode_requireSSL)))) {
        auto sslStatus = session->handshakeSSLForEgress(peer).getNoThrow();
        if (!sslStatus.isOK()) {
            return sslStatus;
        }
    }
#endif

    return static_cast<SessionHandle>(std::move(session));
}

template <typename Endpoint>
StatusWith<TransportLayerASIO::ASIOSessionHandle> TransportLayerASIO::_doSyncConnect(
    Endpoint endpoint, const HostAndPort& peer, const Milliseconds& timeout) {
    GenericSocket sock(*_egressReactor);
    std::error_code ec;
    sock.open(endpoint->protocol());
    sock.non_blocking(true);

    auto now = Date_t::now();
    auto expiration = now + timeout;
    do {
        auto curTimeout = expiration - now;
        sock.connect(*endpoint, curTimeout.toSystemDuration(), ec);
        if (ec) {
            now = Date_t::now();
        }
        // We loop below if ec == interrupted to deal with EINTR failures, otherwise we handle
        // the error/timeout below.
    } while (ec == asio::error::interrupted && now < expiration);

    auto status = [&] {
        if (ec) {
            return errorCodeToStatus(ec);
        } else if (now >= expiration) {
            return Status(ErrorCodes::NetworkTimeout, "Timed out");
        } else {
            return Status::OK();
        }
    }();

    if (!status.isOK()) {
        return makeConnectError(status, peer, endpoint);
    }

    sock.non_blocking(false);
    try {
        return std::make_shared<ASIOSession>(this, std::move(sock), false);
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

Future<SessionHandle> TransportLayerASIO::asyncConnect(HostAndPort peer,
                                                       ConnectSSLMode sslMode,
                                                       const ReactorHandle& reactor,
                                                       Milliseconds timeout) {

    struct AsyncConnectState {
        AsyncConnectState(HostAndPort peer,
                          asio::io_context& context,
                          Promise<SessionHandle> promise_)
            : promise(std::move(promise_)),
              socket(context),
              timeoutTimer(context),
              resolver(context),
              peer(std::move(peer)) {}

        AtomicBool done{false};
        Promise<SessionHandle> promise;

        stdx::mutex mutex;
        GenericSocket socket;
        ASIOReactorTimer timeoutTimer;
        WrappedResolver resolver;
        WrappedEndpoint resolvedEndpoint;
        const HostAndPort peer;
        TransportLayerASIO::ASIOSessionHandle session;
    };

    auto reactorImpl = checked_cast<ASIOReactor*>(reactor.get());
    auto pf = makePromiseFuture<SessionHandle>();
    auto connector =
        std::make_shared<AsyncConnectState>(std::move(peer), *reactorImpl, std::move(pf.promise));
    Future<SessionHandle> mergedFuture = std::move(pf.future);

    if (connector->peer.host().empty()) {
        return Status{ErrorCodes::HostNotFound, "Hostname or IP address to connect to is empty"};
    }

    if (timeout > Milliseconds{0} && timeout < Milliseconds::max()) {
        connector->timeoutTimer.waitFor(timeout).getAsync([connector](Status status) {
            if (status == ErrorCodes::CallbackCanceled || connector->done.swap(true)) {
                return;
            }

            connector->promise.setError(
                makeConnectError({ErrorCodes::NetworkTimeout, "Connecting timed out"},
                                 connector->peer,
                                 connector->resolvedEndpoint));

            std::error_code ec;
            stdx::lock_guard<stdx::mutex> lk(connector->mutex);
            connector->resolver.cancel();
            if (connector->session) {
                connector->session->end();
            } else {
                connector->socket.cancel(ec);
            }
        });
    }

    Date_t timeBefore = Date_t::now();

    connector->resolver.asyncResolve(connector->peer, _listenerOptions.enableIPv6)
        .then([connector, timeBefore](WrappedResolver::EndpointVector results) {
            try {
                Date_t timeAfter = Date_t::now();
                if (timeAfter - timeBefore > Seconds(1)) {
                    warning() << "DNS resolution while connecting to " << connector->peer
                              << " took " << timeAfter - timeBefore;
                }

                stdx::lock_guard<stdx::mutex> lk(connector->mutex);

                connector->resolvedEndpoint = results.front();
                connector->socket.open(connector->resolvedEndpoint->protocol());
                connector->socket.non_blocking(true);
            } catch (asio::system_error& ex) {
                return futurize(ex.code());
            }

            return connector->socket.async_connect(*connector->resolvedEndpoint, UseFuture{});
        })
        .then([this, connector, sslMode]() -> Future<void> {
            stdx::unique_lock<stdx::mutex> lk(connector->mutex);
            connector->session =
                std::make_shared<ASIOSession>(this, std::move(connector->socket), false);
            connector->session->ensureAsync();

#ifndef MONGO_CONFIG_SSL
            if (sslMode == kEnableSSL) {
                uasserted(ErrorCodes::InvalidSSLConfiguration, "SSL requested but not supported");
            }
#else
            auto globalSSLMode = _sslMode();
            if (sslMode == kEnableSSL ||
                (sslMode == kGlobalSSLMode && ((globalSSLMode == SSLParams::SSLMode_preferSSL) ||
                                               (globalSSLMode == SSLParams::SSLMode_requireSSL)))) {
                return connector->session
                    ->handshakeSSLForEgressWithLock(std::move(lk), connector->peer)
                    .then([connector] { return Status::OK(); });
            }
#endif
            return Status::OK();
        })
        .onError([connector](Status status) -> Future<void> {
            return makeConnectError(status, connector->peer, connector->resolvedEndpoint);
        })
        .getAsync([connector](Status connectResult) {
            if (MONGO_FAIL_POINT(transportLayerASIOasyncConnectTimesOut)) {
                log() << "asyncConnectTimesOut fail point is active. simulating timeout.";
                return;
            }

            if (connector->done.swap(true)) {
                return;
            }

            connector->timeoutTimer.cancel();
            if (connectResult.isOK()) {
                connector->promise.emplaceValue(std::move(connector->session));
            } else {
                connector->promise.setError(connectResult);
            }
        });

    return mergedFuture;
}

Status TransportLayerASIO::setup() {
    std::vector<std::string> listenAddrs;
    if (_listenerOptions.ipList.empty() && _listenerOptions.isIngress()) {
        listenAddrs = {"127.0.0.1"};
        if (_listenerOptions.enableIPv6) {
            listenAddrs.emplace_back("::1");
        }
    } else if (!_listenerOptions.ipList.empty()) {
        listenAddrs = _listenerOptions.ipList;
    }

#ifndef _WIN32
    if (_listenerOptions.useUnixSockets && _listenerOptions.isIngress()) {
        listenAddrs.emplace_back(makeUnixSockPath(_listenerOptions.port));
    }
#endif

    if (!(_listenerOptions.isIngress()) && !listenAddrs.empty()) {
        return {ErrorCodes::BadValue,
                "Cannot bind to listening sockets with ingress networking is disabled"};
    }

    _listenerPort = _listenerOptions.port;
    WrappedResolver resolver(*_acceptorReactor);

    // Self-deduplicating list of unique endpoint addresses.
    std::set<WrappedEndpoint> endpoints;
    for (auto& ip : listenAddrs) {
        if (ip.empty()) {
            warning() << "Skipping empty bind address";
            continue;
        }

        auto swAddrs =
            resolver.resolve(HostAndPort(ip, _listenerPort), _listenerOptions.enableIPv6);
        if (!swAddrs.isOK()) {
            warning() << "Found no addresses for " << swAddrs.getStatus();
            continue;
        }
        auto& addrs = swAddrs.getValue();
        endpoints.insert(addrs.begin(), addrs.end());
    }

    for (auto& addr : endpoints) {
#ifndef _WIN32
        if (addr.family() == AF_UNIX) {
            if (::unlink(addr.toString().c_str()) == -1 && errno != ENOENT) {
                error() << "Failed to unlink socket file " << addr.toString().c_str() << " "
                        << errnoWithDescription(errno);
                fassertFailedNoTrace(40486);
            }
        }
#endif
        if (addr.family() == AF_INET6 && !_listenerOptions.enableIPv6) {
            error() << "Specified ipv6 bind address, but ipv6 is disabled";
            fassertFailedNoTrace(40488);
        }

        GenericAcceptor acceptor(*_acceptorReactor);
        acceptor.open(addr->protocol());
        acceptor.set_option(GenericAcceptor::reuse_address(true));
        if (addr.family() == AF_INET6) {
            acceptor.set_option(asio::ip::v6_only(true));
        }

        std::error_code ec;
        acceptor.non_blocking(true, ec);
        if (ec) {
            return errorCodeToStatus(ec);
        }

        acceptor.bind(*addr, ec);
        if (ec) {
            return errorCodeToStatus(ec);
        }

#ifndef _WIN32
        if (addr.family() == AF_UNIX) {
            if (::chmod(addr.toString().c_str(), serverGlobalParams.unixSocketPermissions) == -1) {
                error() << "Failed to chmod socket file " << addr.toString().c_str() << " "
                        << errnoWithDescription(errno);
                fassertFailedNoTrace(40487);
            }
        }
#endif
        if (_listenerOptions.port == 0 && (addr.family() == AF_INET || addr.family() == AF_INET6)) {
            if (_listenerPort != _listenerOptions.port) {
                return Status(ErrorCodes::BadValue,
                              "Port 0 (ephemeral port) is not allowed when"
                              " listening on multiple IP interfaces");
            }
            std::error_code ec;
            auto endpoint = acceptor.local_endpoint(ec);
            if (ec) {
                return errorCodeToStatus(ec);
            }
            _listenerPort = endpointToHostAndPort(endpoint).port();
        }

        _acceptors.emplace_back(SockAddr(addr->data(), addr->size()), std::move(acceptor));
    }

    if (_acceptors.empty() && _listenerOptions.isIngress()) {
        return Status(ErrorCodes::SocketException, "No available addresses/ports to bind to");
    }

#ifdef MONGO_CONFIG_SSL
    const auto& sslParams = getSSLGlobalParams();
    auto sslManager = getSSLManager();

    if (_sslMode() != SSLParams::SSLMode_disabled && _listenerOptions.isIngress()) {
        _ingressSSLContext = stdx::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);

        Status status =
            sslManager->initSSLContext(_ingressSSLContext->native_handle(),
                                       sslParams,
                                       SSLManagerInterface::ConnectionDirection::kIncoming);
        if (!status.isOK()) {
            return status;
        }
    }

    if (_listenerOptions.isEgress() && sslManager) {
        _egressSSLContext = stdx::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
        Status status =
            sslManager->initSSLContext(_egressSSLContext->native_handle(),
                                       sslParams,
                                       SSLManagerInterface::ConnectionDirection::kOutgoing);
        if (!status.isOK()) {
            return status;
        }
    }
#endif

    return Status::OK();
}

Status TransportLayerASIO::start() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _running.store(true);

    if (_listenerOptions.isIngress()) {
        for (auto& acceptor : _acceptors) {
            asio::error_code ec;
            acceptor.second.listen(serverGlobalParams.listenBacklog, ec);
            if (ec) {
                severe() << "Error listening for new connections on " << acceptor.first << ": "
                         << ec.message();
                fassertFailed(31339);
            }

            _acceptConnection(acceptor.second);
        }

        _listenerThread = stdx::thread([this] {
            setThreadName("listener");
            while (_running.load()) {
                _acceptorReactor->run();
            }
        });

        const char* ssl = "";
#ifdef MONGO_CONFIG_SSL
        if (_sslMode() != SSLParams::SSLMode_disabled) {
            ssl = " ssl";
        }
#endif
        log() << "waiting for connections on port " << _listenerPort << ssl;
    } else {
        invariant(_acceptors.empty());
    }

    return Status::OK();
}

void TransportLayerASIO::shutdown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _running.store(false);

    // Loop through the acceptors and cancel their calls to async_accept. This will prevent new
    // connections from being opened.
    for (auto& acceptor : _acceptors) {
        acceptor.second.cancel();
        auto& addr = acceptor.first;
        if (addr.getType() == AF_UNIX && !addr.isAnonymousUNIXSocket()) {
            auto path = addr.getAddr();
            log() << "removing socket file: " << path;
            if (::unlink(path.c_str()) != 0) {
                const auto ewd = errnoWithDescription();
                warning() << "Unable to remove UNIX socket " << path << ": " << ewd;
            }
        }
    }

    // If the listener thread is joinable (that is, we created/started a listener thread), then
    // the io_context is owned exclusively by the TransportLayer and we should stop it and join
    // the listener thread.
    //
    // Otherwise the ServiceExecutor may need to continue running the io_context to drain running
    // connections, so we just cancel the acceptors and return.
    if (_listenerThread.joinable()) {
        _acceptorReactor->stop();
        _listenerThread.join();
    }
}

ReactorHandle TransportLayerASIO::getReactor(WhichReactor which) {
    switch (which) {
        case TransportLayer::kIngress:
            return _ingressReactor;
        case TransportLayer::kEgress:
            return _egressReactor;
        case TransportLayer::kNewReactor:
            return std::make_shared<ASIOReactor>();
    }

    MONGO_UNREACHABLE;
}

void TransportLayerASIO::_acceptConnection(GenericAcceptor& acceptor) {
    auto acceptCb = [this, &acceptor](const std::error_code& ec, GenericSocket peerSocket) mutable {
        if (!_running.load())
            return;

        if (ec) {
            log() << "Error accepting new connection on "
                  << endpointToHostAndPort(acceptor.local_endpoint()) << ": " << ec.message();
            _acceptConnection(acceptor);
            return;
        }

        try {
            std::shared_ptr<ASIOSession> session(
                new ASIOSession(this, std::move(peerSocket), true));
            _sep->startSession(std::move(session));
        } catch (const DBException& e) {
            warning() << "Error accepting new connection " << e;
        }

        _acceptConnection(acceptor);
    };

    acceptor.async_accept(*_ingressReactor, std::move(acceptCb));
}

#ifdef MONGO_CONFIG_SSL
SSLParams::SSLModes TransportLayerASIO::_sslMode() const {
    return static_cast<SSLParams::SSLModes>(getSSLGlobalParams().sslMode.load());
}
#endif

BatonHandle TransportLayerASIO::makeBaton(OperationContext* opCtx) {
#ifdef __linux__
    auto baton = std::make_shared<BatonASIO>(opCtx);

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        invariant(!opCtx->getBaton());
        opCtx->setBaton(baton);
    }

    return std::move(baton);
#else
    return nullptr;
#endif
}

}  // namespace transport
}  // namespace mongo
