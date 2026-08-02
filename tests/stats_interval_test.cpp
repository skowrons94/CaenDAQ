// StatsCollector pacing: the first evaluation must arrive early, and a change
// of interval must take effect on the tick already in flight.
//
// Both exist for the operator, not the physics: a 10 s averaging window gives
// smooth trends but would otherwise leave the rate page empty for 10 s after a
// run starts, and shortening the interval from the UI would not be felt until
// the current long tick expired.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "caendaq/StatsCollector.hpp"

using namespace caendaq;
using clk = std::chrono::steady_clock;

static long long msSince(clk::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
}

// A sampler that counts how often it is called and hands back a monotonically
// growing counter, so rates are well-defined.
struct Counter {
    std::atomic<int> calls{0};
    std::uint64_t    n = 0;
    std::vector<BoardSample> operator()() {
        calls.fetch_add(1);
        n += 100;
        BoardSample b;
        b.name = "MOCK";
        b.boardRegId = 0;
        b.bytesWritten = n;
        ChannelSample c;
        c.board = 0; c.ch = 0;
        c.counts.events = n;
        b.channels.push_back(c);
        return {b};
    }
};

int main() {
    // ---- 1. First evaluation is early even with a long interval ------------
    {
        Counter counter;
        StatsCollector::Options o;
        o.intervalMs      = 10000;   // 10 s window, as an operator might pick
        o.firstIntervalMs = 300;     // stands in for the 2 s default
        StatsCollector sc([&counter] { return counter(); }, o);

        const auto t0 = clk::now();
        sc.start();
        // Poll until the first rates appear.
        while (sc.snapshot().empty() && msSince(t0) < 5000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const long long first = msSince(t0);
        printf("first evaluation with interval=10000 first=300: %lld ms\n", first);
        assert(!sc.snapshot().empty() && "the first evaluation must not wait a full interval");
        assert(first < 3000 && "first evaluation must follow firstIntervalMs, not intervalMs");
        sc.stop();
    }

    // ---- 2. firstIntervalMs never SLOWS a short interval --------------------
    {
        Counter counter;
        StatsCollector::Options o;
        o.intervalMs      = 200;
        o.firstIntervalMs = 5000;    // larger than the interval: must be ignored
        StatsCollector sc([&counter] { return counter(); }, o);
        const auto t0 = clk::now();
        sc.start();
        while (sc.snapshot().empty() && msSince(t0) < 3000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const long long first = msSince(t0);
        printf("first evaluation with interval=200 first=5000: %lld ms\n", first);
        assert(first < 1500 && "firstIntervalMs must be clamped to intervalMs");
        sc.stop();
    }

    // ---- 3. Shortening the interval cuts the tick in flight short -----------
    {
        Counter counter;
        StatsCollector::Options o;
        o.intervalMs      = 30000;   // a very long tick to interrupt
        o.firstIntervalMs = 100;
        StatsCollector sc([&counter] { return counter(); }, o);
        sc.start();
        // Let the early first tick happen, so we are now inside a 30 s wait.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const int before = counter.calls.load();

        const auto t0 = clk::now();
        sc.setInterval(150);         // must interrupt the 30 s wait
        while (counter.calls.load() < before + 2 && msSince(t0) < 5000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const long long reacted = msSince(t0);
        printf("two evaluations after shortening 30000 -> 150: %lld ms\n", reacted);
        assert(counter.calls.load() >= before + 2 &&
               "a shortened interval must apply to the wait already in progress");
        assert(reacted < 3000 && "shortening must not wait out the old interval");
        assert(sc.intervalMs() == 150);
        sc.stop();
    }

    // ---- 4. Clamping -------------------------------------------------------
    {
        Counter counter;
        StatsCollector::Options o;
        o.intervalMs = 1000;
        StatsCollector sc([&counter] { return counter(); }, o);
        sc.setInterval(1);
        assert(sc.intervalMs() == StatsCollector::kMinIntervalMs);
        sc.setInterval(99999999);
        assert(sc.intervalMs() == StatsCollector::kMaxIntervalMs);
        printf("clamping: min=%d max=%d\n",
               StatsCollector::kMinIntervalMs, StatsCollector::kMaxIntervalMs);
    }

    // ---- 5. stop() stays prompt regardless of a long interval ---------------
    {
        Counter counter;
        StatsCollector::Options o;
        o.intervalMs      = 60000;
        o.firstIntervalMs = 60000;
        StatsCollector sc([&counter] { return counter(); }, o);
        sc.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto t0 = clk::now();
        sc.stop();
        const long long ms = msSince(t0);
        printf("stop() while inside a 60 s tick: %lld ms\n", ms);
        assert(ms < 1000 && "stop() must not wait out the interval");
    }

    printf("PASS: first evaluation is early, interval changes apply immediately.\n");
    return 0;
}
