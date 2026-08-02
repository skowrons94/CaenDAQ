#pragma once
//
// GraphiteClient — a tiny, portable (POSIX + Windows) plaintext client for a
// Graphite/Carbon server. Best-effort: a failed connection or send is logged
// and retried later; it never throws.
//
// Every call is time-bounded. That is the point of this class rather than an
// incidental property: a blocking connect() to an unreachable host takes the
// kernel's SYN timeout (~130 s on Linux), and the caller is the stats thread,
// so a lost Graphite server would freeze the rate snapshot the API serves,
// stall set_graphite(), and delay run teardown by minutes. Instead the socket
// is opened non-blocking with an explicit timeout, sends carry SO_SNDTIMEO, and
// a failed target is retried on a backoff rather than on every tick.
//
// Not thread-safe: one owner thread calls send(). Ownership may be handed over
// via shared_ptr so the object outlives a concurrent retarget.
//
// Line protocol (Carbon plaintext, one metric per line):
//   <metric.path> <value> <unix_epoch>\n
//
#include <chrono>
#include <string>

namespace caendaq {

class GraphiteClient {
public:
    // Timeouts are deliberately short. Monitoring is expendable; the stats tick
    // that carries it is not, and at the default 1 s interval a push that took
    // longer than this would be overtaken by the next sample anyway.
    static constexpr int kDefaultConnectTimeoutMs = 200;
    static constexpr int kDefaultSendTimeoutMs    = 200;

    GraphiteClient(std::string host, int port,
                   int connectTimeoutMs = kDefaultConnectTimeoutMs,
                   int sendTimeoutMs    = kDefaultSendTimeoutMs);
    ~GraphiteClient();

    GraphiteClient(const GraphiteClient&) = delete;
    GraphiteClient& operator=(const GraphiteClient&) = delete;

    bool enabled() const { return !host_.empty() && port_ > 0; }

    // Send one already-formatted batch of lines. False on I/O error or while
    // backing off from a previous failure; the socket is dropped and reopened
    // on a later call. Bounded by the connect/send timeouts above.
    bool send(const std::string& payload);

    // True while waiting out the backoff after a failure (for tests/logging).
    bool backingOff() const;

private:
    bool connectSocket();
    void closeSocket();
    void noteFailure();   // drop the socket and extend the retry backoff
    void noteSuccess();   // clear the backoff

    using clock = std::chrono::steady_clock;

    // Retry no faster than this after a failure, doubling up to the cap, so an
    // unreachable server costs one short attempt every kRetryMax rather than a
    // connect on every stats tick.
    static constexpr auto kRetryInitial = std::chrono::milliseconds(1000);
    static constexpr auto kRetryMax     = std::chrono::milliseconds(30000);

    std::string host_;
    int         port_;
    int         connectTimeoutMs_;
    int         sendTimeoutMs_;
    long long   fd_ = -1;   // socket handle (int on POSIX, SOCKET on Windows)
    bool        wsaInit_ = false;

    clock::time_point         nextAttempt_{};      // epoch = attempt immediately
    std::chrono::milliseconds retryDelay_{0};
    bool                      warned_ = false;     // log the outage once, not per tick
};

} // namespace caendaq
