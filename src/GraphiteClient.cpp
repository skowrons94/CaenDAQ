#include "caendaq/GraphiteClient.hpp"

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
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using socket_t = int;
#  define CAENDAQ_INVALID_SOCK (-1)
#endif

namespace caendaq {

GraphiteClient::GraphiteClient(std::string host, int port)
    : host_(std::move(host)), port_(port) {
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
#if defined(_WIN32)
    closesocket(static_cast<socket_t>(fd_));
#else
    ::close(static_cast<socket_t>(fd_));
#endif
    fd_ = -1;
}

bool GraphiteClient::connectSocket() {
    if (!enabled()) return false;

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        LOG_WARN("GraphiteClient: cannot resolve " << host_ << ":" << port_);
        return false;
    }

    socket_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == CAENDAQ_INVALID_SOCK) { freeaddrinfo(res); return false; }

    if (connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        freeaddrinfo(res);
#if defined(_WIN32)
        closesocket(s);
#else
        ::close(s);
#endif
        LOG_WARN("GraphiteClient: cannot connect to " << host_ << ":" << port_);
        return false;
    }
    freeaddrinfo(res);
    fd_ = static_cast<long long>(s);
    return true;
}

bool GraphiteClient::send(const std::string& payload) {
    if (!enabled() || payload.empty()) return true;
    if (fd_ == -1 && !connectSocket()) return false;

    const char* data = payload.data();
    std::size_t remaining = payload.size();
    while (remaining > 0) {
#if defined(_WIN32)
        int n = ::send(static_cast<socket_t>(fd_), data, static_cast<int>(remaining), 0);
#else
        ssize_t n = ::send(static_cast<socket_t>(fd_), data, remaining, 0);
#endif
        if (n <= 0) {
            closeSocket();          // drop; reconnect on the next push
            return false;
        }
        data += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace caendaq
