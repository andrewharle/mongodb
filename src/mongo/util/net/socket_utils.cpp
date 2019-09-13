
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

#include "mongo/util/net/socket_utils.h"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#if defined(__OpenBSD__)
#include <sys/uio.h>
#endif
#else
#include <mstcpip.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "mongo/db/server_options.h"
#include "mongo/util/concurrency/value.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/winutil.h"

namespace mongo {

#if defined(_WIN32)
const struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
            log() << "ERROR: wsastartup failed " << errnoWithDescription();
            quickExit(EXIT_NTSERVICE_ERROR);
        }
    }
} winsock_init;
#endif

static bool ipv6 = false;
void enableIPv6(bool state) {
    ipv6 = state;
}
bool IPv6Enabled() {
    return ipv6;
}

#ifdef _WIN32
#ifdef _UNICODE
#define X_STR_CONST(str) (L##str)
#else
#define X_STR_CONST(str) (str)
#endif
const CString kKeepAliveGroup(
    X_STR_CONST("SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters"));
const CString kKeepAliveTime(X_STR_CONST("KeepAliveTime"));
const CString kKeepAliveInterval(X_STR_CONST("KeepAliveInterval"));
#undef X_STR_CONST
#endif

void setSocketKeepAliveParams(int sock,
                              unsigned int maxKeepIdleSecs,
                              unsigned int maxKeepIntvlSecs) {
#ifdef _WIN32
    // Defaults per MSDN when registry key does not exist.
    // Expressed in seconds here to be consistent with posix,
    // though Windows uses milliseconds.
    const DWORD kWindowsKeepAliveTimeSecsDefault = 2 * 60 * 60;
    const DWORD kWindowsKeepAliveIntervalSecsDefault = 1;

    const auto getKey = [](const CString& key, DWORD default_value) {
        auto withval = windows::getDWORDRegistryKey(kKeepAliveGroup, key);
        if (withval.isOK()) {
            auto val = withval.getValue();
            // Return seconds
            return val ? (val.get() / 1000) : default_value;
        }
        error() << "can't get KeepAlive parameter: " << withval.getStatus();
        return default_value;
    };

    const auto keepIdleSecs = getKey(kKeepAliveTime, kWindowsKeepAliveTimeSecsDefault);
    const auto keepIntvlSecs = getKey(kKeepAliveInterval, kWindowsKeepAliveIntervalSecsDefault);

    if ((keepIdleSecs > maxKeepIdleSecs) || (keepIntvlSecs > maxKeepIntvlSecs)) {
        DWORD sent = 0;
        struct tcp_keepalive keepalive;
        keepalive.onoff = TRUE;
        keepalive.keepalivetime = std::min<DWORD>(keepIdleSecs, maxKeepIdleSecs) * 1000;
        keepalive.keepaliveinterval = std::min<DWORD>(keepIntvlSecs, maxKeepIntvlSecs) * 1000;
        if (WSAIoctl(sock,
                     SIO_KEEPALIVE_VALS,
                     &keepalive,
                     sizeof(keepalive),
                     nullptr,
                     0,
                     &sent,
                     nullptr,
                     nullptr)) {
            error() << "failed setting keepalive values: " << WSAGetLastError();
        }
    }
#elif defined(__APPLE__) || defined(__linux__)
    const auto updateSockOpt =
        [sock](int level, int optnum, unsigned int maxval, StringData optname) {
            unsigned int optval = 1;
            socklen_t len = sizeof(optval);

            if (getsockopt(sock, level, optnum, (char*)&optval, &len)) {
                error() << "can't get " << optname << ": " << errnoWithDescription();
            }

            if (optval > maxval) {
                optval = maxval;
                if (setsockopt(sock, level, optnum, (char*)&optval, sizeof(optval))) {
                    error() << "can't set " << optname << ": " << errnoWithDescription();
                }
            }
        };

#ifdef __APPLE__
    updateSockOpt(IPPROTO_TCP, TCP_KEEPALIVE, maxKeepIdleSecs, "TCP_KEEPALIVE");
#endif

#ifdef __linux__
#ifdef SOL_TCP
    const int level = SOL_TCP;
#else
    const int level = SOL_SOCKET;
#endif
    updateSockOpt(level, TCP_KEEPIDLE, maxKeepIdleSecs, "TCP_KEEPIDLE");
    updateSockOpt(level, TCP_KEEPINTVL, maxKeepIntvlSecs, "TCP_KEEPINTVL");
#endif

#endif
}

std::string makeUnixSockPath(int port) {
    return mongoutils::str::stream() << serverGlobalParams.socket << "/mongodb-" << port << ".sock";
}

// If an ip address is passed in, just return that.  If a hostname is passed
// in, look up its ip and return that.  Returns "" on failure.
std::string hostbyname(const char* hostname) {
    SockAddr sockAddr(hostname, 0, IPv6Enabled() ? AF_UNSPEC : AF_INET);
    if (!sockAddr.isValid() || sockAddr.getAddr() == "0.0.0.0")
        return "";
    else
        return sockAddr.getAddr();
}

//  --- my --

DiagStr& _hostNameCached = *(new DiagStr);  // this is also written to from commands/cloud.cpp

std::string getHostName() {
    char buf[256];
    int ec = gethostname(buf, 127);
    if (ec || *buf == 0) {
        log() << "can't get this server's hostname " << errnoWithDescription();
        return "";
    }
    return buf;
}

/** we store our host name once */
std::string getHostNameCached() {
    std::string temp = _hostNameCached.get();
    if (_hostNameCached.empty()) {
        temp = getHostName();
        _hostNameCached = temp;
    }
    return temp;
}

std::string getHostNameCachedAndPort() {
    return str::stream() << getHostNameCached() << ':' << serverGlobalParams.port;
}

std::string prettyHostName() {
    return (serverGlobalParams.port == ServerGlobalParams::DefaultDBPort
                ? getHostNameCached()
                : getHostNameCachedAndPort());
}

}  // namespace mongo
