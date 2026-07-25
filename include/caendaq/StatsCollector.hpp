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
    double writeRate = 0;         // bytes / second to file
    std::uint64_t boardFail = 0;  // cumulative board-FAIL aggregates this run
    std::vector<ChannelRate> channels;
};

class StatsCollector {
public:
    struct Options {
        int         intervalMs   = 1000;
        std::string graphiteHost;              // empty = no Graphite push
        int         graphitePort = 2003;
        std::string prefix       = "ancillary.rates";
    };

    StatsCollector(SampleFn sampler, Options opt);
    ~StatsCollector();

    void start();
    void stop();

    // Change the Graphite/Carbon target while running (empty host disables it).
    void setGraphite(const std::string& host, int port);

    std::vector<BoardRate> snapshot() const;

private:
    void loop();
    std::string formatGraphite(const std::vector<BoardRate>& rates, long long epoch) const;

    SampleFn                        sampler_;
    Options                         opt_;
    mutable std::mutex              graphiteMtx_;
    std::unique_ptr<GraphiteClient> graphite_;

    std::thread             thread_;
    std::atomic<bool>       running_{false};
    std::condition_variable cv_;
    std::mutex              cvMtx_;

    mutable std::mutex      snapMtx_;
    std::vector<BoardRate>  latest_;
};

} // namespace caendaq
