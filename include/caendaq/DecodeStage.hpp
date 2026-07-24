#pragma once
//
// DecodeStage — the parallel, non-blocking decode tap.
//
// It runs on its own thread and consumes raw blocks from a bounded best-effort
// queue: the reader submits each block to BOTH the disk writer (must not drop)
// and here (may drop under load). So decoding/histogramming never slows the
// readout or the file writing — if the decoder can't keep up, spectra briefly
// lag but no data is lost to disk.
//
#include <atomic>
#include <cstdint>
#include <thread>

#include "caendaq/AggregateDecoder.hpp"
#include "caendaq/BlockQueue.hpp"
#include "caendaq/BoardInfo.hpp"
#include "caendaq/HistogramStore.hpp"
#include "caendaq/RawBlock.hpp"

namespace caendaq {

class DecodeStage {
public:
    DecodeStage(const BoardInfo& info, HistogramStore& store, std::size_t queueCapacity = 1024);

    void start();
    // Best-effort: drops (counted) if the decode queue is full, never blocks.
    void submit(const BlockPtr& block);
    void stop();

    std::uint64_t eventsDecoded() const { return events_.load(); }
    std::uint64_t blocksDropped() const { return queue_.dropped(); }
    std::uint64_t corruptAggregates() const { return decoder_.corruptCount(); }

private:
    void loop();

    AggregateDecoder decoder_;
    HistogramStore&  store_;
    BlockQueue       queue_;
    std::thread      thread_;
    std::atomic<bool>          running_{false};
    std::atomic<std::uint64_t> events_{0};
};

} // namespace caendaq
