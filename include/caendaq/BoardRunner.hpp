#pragma once
//
// BoardRunner — drives one board through a run and streams its raw buffers to
// disk on a two-thread pipeline:
//
//     [reader thread]  digitizer.read()  --RawBlock-->  BlockQueue
//     [writer thread]  BlockQueue.pop()  -------------> RawFileWriter (.caendat)
//
// The reader stays tight and time-critical (only read() + enqueue); all disk
// I/O happens on the writer thread so a stalling filesystem can't back up the
// board. Every thread body is wrapped so no exception can escape and crash the
// process.
//
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "caendaq/BlockQueue.hpp"
#include "caendaq/DecodeStage.hpp"
#include "caendaq/HistogramStore.hpp"
#include "caendaq/IDigitizer.hpp"
#include "caendaq/RawFileWriter.hpp"

namespace caendaq {

struct BoardRunnerConfig {
    WriterConfig  writer;                 // output file settings for this board
    std::size_t   queueCapacity = 4096;   // max in-flight raw blocks (writer path)
    int           reconnectBackoffMs = 500; // pause after a comm error before retry
    bool          write  = true;           // write .caendat files (false = decode-only)
    bool          decode = false;          // enable the parallel decode tap
    std::size_t   decodeQueueCapacity = 1024;
};

class BoardRunner {
public:
    BoardRunner(std::unique_ptr<IDigitizer> dgtz, BoardRunnerConfig cfg);
    ~BoardRunner();

    // Open + configure the board and open the output file. False on failure.
    bool prepare();

    // Start acquisition and spawn the reader/writer threads.
    bool start();

    // Stop acquisition, drain the queue, flush and close the file. Idempotent.
    void stop();

    // Live counters (safe to read from any thread).
    std::uint64_t buffersRead()  const { return buffersRead_.load(); }
    std::uint64_t bytesRead()    const { return bytesRead_.load(); }
    std::uint64_t bytesWritten() const { return writer_.bytesWritten(); }
    std::uint64_t blocksDropped() const { return queue_.dropped(); }
    std::uint64_t commErrors()   const { return commErrors_.load(); }
    const std::string& name()    const { return dgtz_->name(); }

    // Decode tap (present only when cfg.decode is set).
    bool          decoding()       const { return decodeStage_ != nullptr; }
    std::uint64_t eventsDecoded()  const { return decodeStage_ ? decodeStage_->eventsDecoded() : 0; }
    std::uint64_t decodeDropped()  const { return decodeStage_ ? decodeStage_->blocksDropped() : 0; }
    std::uint64_t boardFailures()  const { return decodeStage_ ? decodeStage_->boardFailures() : 0; }
    HistogramStore&       histograms()       { return histograms_; }
    const HistogramStore& histograms() const { return histograms_; }

private:
    void readerLoop();
    void writerLoop();

    std::unique_ptr<IDigitizer> dgtz_;
    BoardRunnerConfig           cfg_;
    RawFileWriter               writer_;
    BlockQueue                  queue_;
    HistogramStore              histograms_;
    std::unique_ptr<DecodeStage> decodeStage_;

    std::thread        readerThread_;
    std::thread        writerThread_;
    std::atomic<bool>  running_{false};
    std::atomic<bool>  writerFailed_{false};

    std::atomic<std::uint64_t> buffersRead_{0};
    std::atomic<std::uint64_t> bytesRead_{0};
    std::atomic<std::uint64_t> commErrors_{0};
};

} // namespace caendaq
