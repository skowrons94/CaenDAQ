// GraphiteClient must stay time-bounded when the Carbon server is unreachable.
//
// This is the property the DAQ actually depends on: the client is driven by the
// stats thread, so an unbounded connect() would freeze the rate snapshot the API
// serves, stall set_graphite(), and delay run teardown. A plain blocking
// connect() to a black-holed address takes the kernel's SYN timeout (~130 s on
// Linux), which is why the timeouts exist and why they are tested.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>

#include "caendaq/GraphiteClient.hpp"

using namespace caendaq;
using clock_t_ = std::chrono::steady_clock;

static long long msSince(clock_t_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_t_::now() - t0).count();
}

int main() {
    // 203.0.113.0/24 is TEST-NET-3 (RFC 5737): guaranteed never routed on the
    // public internet, so the connect goes nowhere. A literal IP also keeps
    // getaddrinfo() out of the measurement.
    const std::string blackhole = "203.0.113.1";
    const std::string payload   = "test.metric 1 1700000000\n";

    // ---- 1. First push against a dead server returns quickly ---------------
    {
        GraphiteClient c(blackhole, 2003, /*connectTimeoutMs=*/200, /*sendTimeoutMs=*/200);
        const auto t0 = clock_t_::now();
        const bool ok = c.send(payload);
        const long long elapsed = msSince(t0);
        printf("first push to black hole: ok=%d elapsed=%lld ms\n", (int)ok, elapsed);

        assert(!ok && "a push to an unreachable server must report failure");
        // Generous ceiling: the timeout is 200 ms, anything near the old
        // blocking behaviour (tens of seconds) fails loudly.
        assert(elapsed < 3000 && "connect must be bounded by the timeout, not the SYN timeout");
    }

    // ---- 2. Subsequent pushes are ~free while backing off ------------------
    {
        GraphiteClient c(blackhole, 2003, 200, 200);
        c.send(payload);                       // trips the backoff
        assert(c.backingOff() && "a failed push must arm the retry backoff");

        const auto t0 = clock_t_::now();
        for (int i = 0; i < 20; ++i) assert(!c.send(payload));
        const long long elapsed = msSince(t0);
        printf("20 pushes while backing off: elapsed=%lld ms\n", elapsed);

        // These must not touch the network at all, so they should be ~0 ms.
        assert(elapsed < 200 && "pushes during backoff must not attempt to connect");
    }

    // ---- 3. A disabled client is a no-op, never an error --------------------
    {
        GraphiteClient off("", 2003);
        assert(!off.enabled());
        const auto t0 = clock_t_::now();
        assert(off.send(payload) && "a disabled client reports success (nothing to do)");
        assert(msSince(t0) < 200);
    }

    // ---- 4. An empty payload never opens a socket ---------------------------
    {
        GraphiteClient c(blackhole, 2003, 200, 200);
        const auto t0 = clock_t_::now();
        assert(c.send("") && "an empty batch is a no-op");
        assert(msSince(t0) < 200 && "an empty batch must not connect");
        assert(!c.backingOff() && "a no-op must not arm the backoff");
    }

    printf("PASS: GraphiteClient stays bounded and backs off when the server is unreachable.\n");
    return 0;
}
