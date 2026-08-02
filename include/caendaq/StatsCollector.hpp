#pragma once
//
// StatsCollector — computes per-channel and per-board RATES from the cumulative
// counters, on its own thread (like LunaSpy's Stats). Every interval it samples
// the counters, differences them over the elapsed time, stores the latest rates
// for WebDAQ to read, and — independently — pushes them to a Graphite/Carbon
// server if one is configured.
//
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "caendaq/GraphiteClient.hpp"
#include "caendaq/HistogramStore.hpp"

namespace caendaq {

// --- what the collector samples (provided by Daq via a callback) ---
struct ChannelSample {
    std::uint16_t board = 0;
    std::uint16_t ch    = 0;
    HistogramStore::Counts counts;
};
struct BoardSample {
    std::string   name;
    std::uint16_t boardRegId   = 0;   // VME board id (0xEF08) — the 'bo_' in metric paths
    std::uint64_t bytesWritten = 0;
    std::uint64_t boardFail    = 0;   // cumulative board-FAIL aggregates this run
    std::vector<ChannelSample> channels;
};
using SampleFn = std::function<std::vector<BoardSample>()>;

// --- what it produces (rates, per second) ---
struct ChannelRate {
    std::uint16_t board = 0;
    std::uint16_t ch    = 0;
    double events = 0, pileup = 0, lost = 0, satu = 0;   // counts / second
};
struct BoardRate {
    std::string name;
    std::uint16_t boardRegId = 0; // VME board id (0xEF08) — the 'bo_' in metric paths
    double writeRate = 0;         // bytes / second to file
    std::uint64_t boardFail = 0;  // cumulative board-FAIL aggregates this run
    std::vector<ChannelRate> channels;
};

class StatsCollector {
public:
    struct Options {
        // The sampling period, and therefore ALSO the averaging window and the
        // Graphite push period — one tick samples, differences, publishes and
        // pushes, so these cannot be set apart (see loop()).
        int         intervalMs   = 1000;
        // How long the FIRST evaluation waits. A long intervalMs is good for
        // smooth trends but leaves an operator staring at an empty rate page for
        // that long after starting a run, so the first tick comes early and the
        // rest settle to intervalMs. Clamped to intervalMs, so a short interval
        // is never slowed down by this.
        int         firstIntervalMs = 2000;
        std::string graphiteHost;              // empty = no Graphite push
        int         graphitePort = 2003;
        // Root of every metric path this collector writes. One experiment per
        // subtree — 'ancillary.rates.12c12c' and 'ancillary.rates.BGO' keep two
        // campaigns apart in the same Graphite. Boards appear below it as
        // 'bo_<VME board id>', so the prefix is per-experiment, never per-board.
        std::string prefix       = "ancillary.rates";
    };

    StatsCollector(SampleFn sampler, Options opt);
    ~StatsCollector();

    void start();
    void stop();

    // Change the Graphite/Carbon target while running (empty host disables it).
    // An empty prefix leaves the current one alone.
    void setGraphite(const std::string& host, int port, const std::string& prefix = "");

    // Change the sampling period while running. Takes effect immediately — the
    // tick currently being waited out is cut short — so an operator who shortens
    // the interval does not sit through the old one first. Values below
    // kMinIntervalMs are clamped; sampling faster than that costs more than the
    // numbers are worth. Safe from any thread.
    void setInterval(int ms);
    int  intervalMs() const { return intervalMs_.load(std::memory_order_relaxed); }

    static constexpr int kMinIntervalMs = 100;
    static constexpr int kMaxIntervalMs = 600000;   // 10 min

    // Public so every layer that accepts an interval (Daq, the bindings, the
    // web API) clamps to exactly the same range rather than each inventing one.
    static int clampInterval(int ms) {
        return ms < kMinIntervalMs ? kMinIntervalMs
             : ms > kMaxIntervalMs ? kMaxIntervalMs
                                   : ms;
    }

    std::vector<BoardRate> snapshot() const;

private:
    void loop();
    std::string formatGraphite(const std::vector<BoardRate>& rates,
                               const std::string& prefix, long long epoch) const;

    SampleFn                        sampler_;
    Options                         opt_;
    mutable std::mutex              graphiteMtx_;
    // shared_ptr, not unique_ptr: loop() takes a reference under the lock and
    // sends with it released, so setGraphite() can retarget concurrently
    // without destroying a client that is mid-write.
    std::shared_ptr<GraphiteClient> graphite_;

    // Atomic rather than guarded by cvMtx_: loop() reads it every tick and
    // setInterval() writes it from the API thread, and neither needs the other's
    // lock — the condition_variable only has to be notified so a wait already in
    // progress re-evaluates against the new value.
    std::atomic<int>        intervalMs_{1000};
    // Bumped by setInterval() so a wait already in flight can tell "the interval
    // changed" from a spurious wake-up and recompute its deadline.
    std::atomic<unsigned>   intervalGen_{0};

    std::thread             thread_;
    std::atomic<bool>       running_{false};
    std::condition_variable cv_;
    std::mutex              cvMtx_;

    mutable std::mutex      snapMtx_;
    std::vector<BoardRate>  latest_;
};

} // namespace caendaq
