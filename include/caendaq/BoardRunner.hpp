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
    // Pause after a read that returned nothing. Not a throughput knob: the
    // boards behind one bridge take turns on a single link mutex, and a reader
    // that loops straight back into the next MBLT holds that link almost
    // continuously while its board has no data to give — starving the boards
    // that do. Standing aside for a moment when there was nothing to read is
    // what keeps the turn-taking fair. A board with data never gets here.
    int           idlePollMs = 1;
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

    // Is this board configured to wait for an external start signal? Read from
    // its own Acquisition Control register; valid after prepare().
    bool synchronised() const { return dgtz_->synchronised(); }
    std::uint32_t startMode() const { return dgtz_->startMode(); }

    // Start acquisition (software start). prepare() must have run. Does NOT
    // spawn the reader — call startReader() once the whole chain is going.
    bool start();

    // Arm a synchronised board, which then produces nothing until its external
    // start arrives. Does NOT spawn the reader either, for the same reason.
    bool arm();

    // Spawn the reader (and the decode tap). Must be called only after every
    // board is actually acquiring — i.e. after the master's software trigger.
    //
    // The reader cannot be brought up at arm() time, tempting as it is to have
    // the pipeline already draining when the chain fires: a board that is armed
    // but not yet started has no data, so its blocking MBLT read sits in the
    // driver holding the lock that serialises the link. Boards sharing one
    // USB/optical bridge then deadlock outright — the master can never get the
    // link to send the software trigger, so no data ever arrives and the read
    // never returns. Deferring costs only the microseconds until the first
    // read; the board buffers meanwhile, so nothing is lost.
    void startReader();

    // Fire the software trigger that starts a daisy chain (master only).
    bool sendSWTrigger() { return dgtz_->sendSWTrigger(); }

    // Online tuning: change a register on this board while it is being read out.
    // Safe to call from another thread — the backend serialises it against the
    // reader. Which registers may be touched mid-run is decided by the caller.
    bool writeRegister(std::uint32_t address, std::uint32_t value) {
        return dgtz_->writeRegister(address, value);
    }
    bool readRegister(std::uint32_t address, std::uint32_t* value) {
        return dgtz_->readRegister(address, value);
    }

    // Stop acquisition, join the reader, stop the decode tap. Idempotent. Does
    // NOT touch the shared file writer — Daq owns that.
    void stop();

    // Board identity, valid after prepare(). Used to build the file header.
    const BoardInfo& boardInfo() const { return boardInfo_; }

    // Record the role this board ended up with in the chain (set by Daq::start()
    // once the master is known). Reported through boardInfo() for the metadata.
    void setSyncRole(SyncRole role) { boardInfo_.syncRole = role; }

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
