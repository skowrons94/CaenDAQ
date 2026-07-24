#pragma once
//
// GraphiteClient — a tiny, portable (POSIX + Windows) plaintext client for a
// Graphite/Carbon server. Best-effort: a failed connection or send is logged
// and retried on the next push; it never throws and never blocks the DAQ.
//
// Line protocol (Carbon plaintext, one metric per line):
//   <metric.path> <value> <unix_epoch>\n
//
#include <string>

namespace caendaq {

class GraphiteClient {
public:
    GraphiteClient(std::string host, int port);
    ~GraphiteClient();

    GraphiteClient(const GraphiteClient&) = delete;
    GraphiteClient& operator=(const GraphiteClient&) = delete;

    bool enabled() const { return !host_.empty() && port_ > 0; }

    // Send one already-formatted batch of lines. Returns false on I/O error
    // (the socket is dropped and reconnected on the next call).
    bool send(const std::string& payload);

private:
    bool connectSocket();
    void closeSocket();

    std::string host_;
    int         port_;
    long long   fd_ = -1;   // socket handle (int on POSIX, SOCKET on Windows)
    bool        wsaInit_ = false;
};

} // namespace caendaq
