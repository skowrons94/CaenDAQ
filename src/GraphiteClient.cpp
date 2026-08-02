#include "caendaq/GraphiteClient.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "caendaq/Log.hpp"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "Ws2_32.lib")
   using socket_t = SOCKET;
#  define CAENDAQ_INVALID_SOCK INVALID_SOCKET
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define CAENDAQ_INVALID_SOCK (-1)
   // Linux spells "do not raise SIGPIPE" as a send() flag; macOS/BSD have no
   // such flag and use the SO_NOSIGPIPE socket option instead (set on connect).
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
#endif

namespace caendaq {

namespace {

void closeNative(socket_t s) {
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

// Put the socket in (non-)blocking mode. False if the mode could not be set,
// in which case the caller must give up rather than risk a blocking connect.
bool setNonBlocking(socket_t s, bool nonBlocking) {
#if defined(_WIN32)
    u_long mode = nonBlocking ? 1u : 0u;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    const int want = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, want) == 0;
#endif
}

// Wait for the socket to become writable, i.e. for a non-blocking connect to
// settle. Returns 1 ready, 0 timed out, -1 error.
int waitWritable(socket_t s, int timeoutMs) {
#if defined(_WIN32)
    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(s, &wr);
    timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int r = select(0, nullptr, &wr, nullptr, &tv);
    return r > 0 ? 1 : r;
#else
    // poll(), not select(): select() cannot represent a descriptor >= FD_SETSIZE
    // and a long-running DAQ can easily hold that many files open.
    struct pollfd p;
    p.fd      = s;
    p.events  = POLLOUT;
    p.revents = 0;
    int r;
    do {
        r = poll(&p, 1, timeoutMs);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) return r;
    return (p.revents & POLLOUT) ? 1 : -1;
#endif
}

bool lastErrorWasInProgress() {
#if defined(_WIN32)
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EINPROGRESS;
#endif
}

bool lastErrorWasTimeout() {
#if defined(_WIN32)
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

} // namespace

GraphiteClient::GraphiteClient(std::string host, int port,
                               int connectTimeoutMs, int sendTimeoutMs)
    : host_(std::move(host)),
      port_(port),
      connectTimeoutMs_(connectTimeoutMs > 0 ? connectTimeoutMs : kDefaultConnectTimeoutMs),
      sendTimeoutMs_(sendTimeoutMs > 0 ? sendTimeoutMs : kDefaultSendTimeoutMs) {
#if defined(_WIN32)
    if (enabled()) {
        WSADATA wsa;
        wsaInit_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
#endif
}

GraphiteClient::~GraphiteClient() {
    closeSocket();
#if defined(_WIN32)
    if (wsaInit_) WSACleanup();
#endif
}

void GraphiteClient::closeSocket() {
    if (fd_ == -1) return;
    closeNative(static_cast<socket_t>(fd_));
    fd_ = -1;
}

bool GraphiteClient::backingOff() const {
    return retryDelay_.count() > 0 && clock::now() < nextAttempt_;
}

void GraphiteClient::noteFailure() {
    closeSocket();
    retryDelay_ = (retryDelay_.count() == 0)
                      ? kRetryInitial
                      : std::min(retryDelay_ * 2, std::chrono::milliseconds(kRetryMax));
    nextAttempt_ = clock::now() + retryDelay_;
    if (!warned_) {
        LOG_WARN("GraphiteClient: " << host_ << ":" << port_
                 << " unreachable — metrics dropped, retrying with backoff "
                    "(acquisition is unaffected)");
        warned_ = true;   // stay quiet until it comes back
    }
}

void GraphiteClient::noteSuccess() {
    if (warned_) {
        LOG_INFO("GraphiteClient: " << host_ << ":" << port_ << " reachable again");
        warned_ = false;
    }
    retryDelay_ = std::chrono::milliseconds(0);
    nextAttempt_ = clock::time_point{};
}

bool GraphiteClient::connectSocket() {
    if (!enabled()) return false;

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
#ifdef AI_ADDRCONFIG
    hints.ai_flags    = AI_ADDRCONFIG;
#endif

    // NOTE: getaddrinfo() has no portable timeout. For a literal IP it returns
    // immediately; for a name with an unreachable DNS server it can take the
    // resolver timeout (a few seconds). The backoff below is what bounds the
    // cost of that case — it is attempted once per retry window, not per tick.
    // Prefer an IP address in the Graphite host setting on a DAQ machine.
    struct addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return false;
    }

    socket_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == CAENDAQ_INVALID_SOCK) { freeaddrinfo(res); return false; }

    // Non-blocking for the connect, so an unreachable host costs
    // connectTimeoutMs_ instead of the kernel's SYN timeout.
    if (!setNonBlocking(s, true)) { closeNative(s); freeaddrinfo(res); return false; }

    bool connected = false;
    if (connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0) {
        connected = true;                       // succeeded straight away (loopback)
    } else if (lastErrorWasInProgress()) {
        if (waitWritable(s, connectTimeoutMs_) == 1) {
            // Writable also happens on failure — ask the socket how it went.
            int       err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&err), &len) == 0 && err == 0) {
                connected = true;
            }
        }
    }
    freeaddrinfo(res);

    if (!connected) { closeNative(s); return false; }

    // Back to blocking, but with a send timeout so a stalled receiver cannot
    // park the stats thread in ::send() indefinitely.
    if (!setNonBlocking(s, false)) { closeNative(s); return false; }
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(sendTimeoutMs_);
#else
    struct timeval tv;
    tv.tv_sec  = sendTimeoutMs_ / 1000;
    tv.tv_usec = (sendTimeoutMs_ % 1000) * 1000;
#endif
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));

#if defined(SO_NOSIGPIPE)
    // macOS/BSD equivalent of MSG_NOSIGNAL: a Carbon server that goes away must
    // not kill the process with SIGPIPE.
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    fd_ = static_cast<long long>(s);
    return true;
}

bool GraphiteClient::send(const std::string& payload) {
    if (!enabled() || payload.empty()) return true;

    // Still serving a previous failure: skip without touching the network, so a
    // dead server costs nothing per tick.
    if (fd_ == -1 && backingOff()) return false;

    if (fd_ == -1 && !connectSocket()) { noteFailure(); return false; }

    const char* data = payload.data();
    std::size_t remaining = payload.size();
    while (remaining > 0) {
#if defined(_WIN32)
        int n = ::send(static_cast<socket_t>(fd_), data, static_cast<int>(remaining), 0);
#else
        // MSG_NOSIGNAL: a Carbon server that went away must not deliver SIGPIPE
        // and kill the process — this is monitoring, it is allowed to fail.
        ssize_t n = ::send(static_cast<socket_t>(fd_), data, remaining, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            // Timeout or hard error alike: drop the socket, back off, move on.
            // A partial batch is fine, Carbon parses whole lines and the next
            // tick resends the current values anyway.
            const bool timedOut = (n < 0) && lastErrorWasTimeout();
            if (timedOut) {
                LOG_WARN("GraphiteClient: send to " << host_ << ":" << port_
                         << " timed out after " << sendTimeoutMs_ << " ms — dropping batch");
            }
            noteFailure();
            return false;
        }
        data += n;
        remaining -= static_cast<std::size_t>(n);
    }
    noteSuccess();
    return true;
}

} // namespace caendaq
