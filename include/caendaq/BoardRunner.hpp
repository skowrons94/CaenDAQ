#pragma once
//
// BoardRunner — drives ONE board through a run. It reads the board on a tight,
// time-critical reader thread and fans each raw buffer out to two consumers:
//
//     [reader thread]  digitizer.read()  --RawBlock--> shared write queue (Daq)
//                                                   \-> DecodeStage (per board)
//
// The file writing itself is NOT owned here: every board pushes to the SINGLE
// shared BlockQueue owned by Daq, which a single writer thread drains into one
// unified .caendat file (mirroring the XDAQ ReadoutUnit, which reads a vector
// of boards into one file). Decoding stays per board (its own HistogramStore).
// Every thread body is wrapped so no exception can escape and crash the process.
//
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "caendaq/BlockQueue.hpp"
#include "caendaq/BoardInfo.hpp"
#include "caendaq/DecodeStage.hpp"
#include "caendaq/HistogramStore.hpp"
#include "caendaq/IDigitizer.hpp"

namespace caendaq {

struct BoardRunnerConfig {
    int           reconnectBackoffMs = 500; // pause after a comm error before retry
    bool          write  = true;           // push this board's buffers to the file
    bool          decode = false;          // enable the parallel decode tap
    std::size_t   decodeQueueCapacity = 1024;
};

class BoardRunner {
public:
    // writeQueue: the shared queue this board pushes raw buffers to (nullptr when
    //   cfg.write is false / no file is being written).
    // writerFailed: shared flag set by Daq's writer thread on I/O error, so this
    //   board's reader stops pushing into a dead file (may be nullptr).
    BoardRunner(std::unique_ptr<IDigitizer> dgtz, BoardRunnerConfig cfg,
                BlockQueue* writeQueue, std::atomic<bool>* writerFailed);
    ~BoardRunner();

    // Open + configure the board (and set up the decode tap). False on failure.
    // Captures the board identity for the file header (see boardInfo()).
    bool prepare();

    // Start acquisition and spawn the reader thread. prepare() must have run.
    bool start();

    // Stop acquisition, join the reader, stop the decode tap. Idempotent. Does
    // NOT touch the shared file writer — Daq owns that.
    void stop();

    // Board identity, valid after prepare(). Used to build the file header.
    const BoardInfo& boardInfo() const { return boardInfo_; }

    // Live counters (safe to read from any thread).
    std::uint64_t buffersRead()  const { return buffersRead_.load(); }
    std::uint64_t bytesRead()    const { return bytesRead_.load(); }
    std::uint64_t bytesWritten() const { return bytesWritten_.load(); }   // bytes accepted by the write queue
    std::uint64_t blocksDropped() const { return blocksDropped_.load(); } // write-queue rejects (full)
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

    std::unique_ptr<IDigitizer> dgtz_;
    BoardRunnerConfig           cfg_;
    BlockQueue*                 writeQueue_   = nullptr; // shared, owned by Daq
    std::atomic<bool>*          writerFailed_ = nullptr; // shared, owned by Daq
    BoardInfo                   boardInfo_;
    HistogramStore              histograms_;
    std::unique_ptr<DecodeStage> decodeStage_;

    std::thread        readerThread_;
    std::atomic<bool>  running_{false};

    std::atomic<std::uint64_t> buffersRead_{0};
    std::atomic<std::uint64_t> bytesRead_{0};
    std::atomic<std::uint64_t> bytesWritten_{0};
    std::atomic<std::uint64_t> blocksDropped_{0};
    std::atomic<std::uint64_t> commErrors_{0};
};

} // namespace caendaq
