#pragma once
//
// Daq — top-level orchestrator over one or more boards. Each board runs its own
// reader(/decode) pipeline (BoardRunner), but ALL boards share ONE file writer:
// their raw buffers are pushed to a single queue drained by one writer thread
// into one unified .caendat (like the XDAQ ReadoutUnit's multi-board file).
// This is the object the Python (pybind11) layer wraps as an instance.
//
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "caendaq/BlockQueue.hpp"
#include "caendaq/BoardRunner.hpp"
#include "caendaq/HistogramStore.hpp"
#include "caendaq/IDigitizer.hpp"
#include "caendaq/MockDigitizer.hpp"
#include "caendaq/RawFileWriter.hpp"
#include "caendaq/StatsCollector.hpp"

namespace caendaq {

struct BoardSpec {
    BoardParams   params;                 // name, connType, link, node, vmeBase, configPath
    bool          mock       = false;      // use MockDigitizer instead of a real board
    bool          decode     = false;      // enable the parallel decode tap
    bool          write      = true;       // include this board in the .caendat file
    std::uint32_t mockRate   = 200;        // mock buffers/s (mock only)
    bool          mockWaveforms = false;   // mock: emit a synthetic trace per event
    std::uint32_t mockFailEvery = 0;       // mock: inject a board-FAIL ~1/N aggregates (0 = never)
    DppType       mockDpp    = DppType::PSD; // mock: firmware to emulate (PHA=energy, PSD=q). From the board's dpp.
    // mock: Start/Stop Mode to report, taken from the board's real 0x8100 so a
    // TEST_FLAG run mirrors the configured chain (see StartMode in IDigitizer).
    std::uint32_t mockStartMode = 0;
};

class Daq {
public:
    struct Options {
        std::uint64_t maxFileBytes  = 0;
        bool          writeHeader   = true;
        std::string   graphiteHost;          // empty = no Graphite push
        int           graphitePort  = 2003;
        // Root of the metric tree, one subtree per experiment (see StatsCollector).
        std::string   graphitePrefix = "ancillary.rates";
        // Sampling period: also the averaging window and the Graphite push
        // period (they are one tick, see StatsCollector).
        int           statsIntervalMs = 1000;
        // How long the first evaluation waits, so a long interval does not leave
        // the rate page empty at the start of a run. Clamped to statsIntervalMs.
        int           statsFirstIntervalMs = 2000;
    };

    // ── Multi-board synchronisation ────────────────────────────────────────
    // There is no option for it: synchronisation is decided by the boards' OWN
    // configuration, exactly as programmed through the WebDAQ dashboard.
    //
    // A board whose Acquisition Control (0x8100) start mode is anything other
    // than "SW controlled" is armed rather than started, and the master (board
    // register id 0) fires a software trigger once every board is armed. That
    // pulse leaves the master on TRG-OUT, enters the next board's TRG-IN, and
    // walks the chain — so all boards share one time origin:
    //
    //   board 0 (MASTER)      board 1              board 2
    //   first-trigger mode    first-trigger mode   first-trigger mode
    //   SendSWTrigger() ──TRG-OUT──> TRG-IN ──TRG-OUT──> TRG-IN
    //
    // Cable TRG-OUT of each board into TRG-IN of the next. Leave a board in
    // "SW controlled" mode to keep it out of the chain — it just starts on its
    // own, and its timestamps are not comparable with the rest.

    Daq(std::string outputDir, std::uint32_t run, Options opt);
    Daq(std::string outputDir, std::uint32_t run)
        : Daq(std::move(outputDir), run, Options{}) {}
    ~Daq();

    Daq(const Daq&) = delete;
    Daq& operator=(const Daq&) = delete;

    // Add a board. Returns its index, or -1 on failure (e.g. real board asked for
    // in a build without CAEN support). Call before start().
    int addBoard(const BoardSpec& spec);

    // Open + configure + open files for all boards. False if any board fails.
    bool prepare();
    // Start acquisition on all boards (calls prepare() if not already done).
    bool start();
    // Stop all boards, flush and close files. Idempotent.
    void stop();

    std::size_t boardCount() const { return runners_.size(); }
    bool        running() const { return running_; }

    // Per-board access. `board` is the add order index.
    const std::string& boardName(int board) const;
    HistogramStore&    histograms(int board);
    // Board identity + firmware + the acquisition registers as actually read
    // back from the board. Valid after prepare(); this is what run metadata
    // should record. Throws nothing; returns a default BoardInfo if out of range.
    BoardInfo          boardInfo(int board) const;

    // Per-board live counters.
    std::uint64_t buffersRead(int board) const;
    std::uint64_t bytesRead(int board) const;
    std::uint64_t bytesWritten(int board) const;
    std::uint64_t blocksDropped(int board) const;
    std::uint64_t commErrors(int board) const;
    std::uint64_t eventsDecoded(int board) const;
    // Cumulative count of board-FAIL aggregates this run (0 = healthy). Resets
    // every run because a fresh Daq is built per run.
    std::uint64_t boardFailures(int board) const;

    // Latest per-board / per-channel rates (events/pileup/lost/satu per second,
    // and file write rate). Empty until a run with decode has been started.
    std::vector<BoardRate> stats() const;

    // Change the Graphite/Carbon target for the running stats collector (empty
    // host disables it) and, optionally, the metric prefix — the experiment's
    // subtree. An empty prefix keeps the current one. No-op if no run is active.
    void setGraphite(const std::string& host, int port, const std::string& prefix = "");

    // Change the statistics sampling period while a run is live. Takes effect on
    // the spot: shortening it cuts the tick already in flight short. Clamped to
    // [StatsCollector::kMinIntervalMs, kMaxIntervalMs].
    void setStatsInterval(int ms);
    int  statsIntervalMs() const;

    // ── Online tuning ──────────────────────────────────────────────────────
    // Write/read a register on a board that this Daq has open, including while
    // the run is live: the board backend serialises the access against its
    // reader thread. `board` is the add order index.
    //
    // CaenDAQ performs the access and nothing more. It does not judge whether a
    // register may be changed during acquisition — that policy belongs to the
    // caller (WebDAQ keeps an allowlist of the parameters that are safe to move
    // while data is being taken).
    //
    // False if the board index is out of range, the board is not open, or the
    // access failed.
    bool writeRegister(int board, std::uint32_t address, std::uint32_t value);
    bool readRegister(int board, std::uint32_t address, std::uint32_t* value);

private:
    std::unique_ptr<IDigitizer> makeDigitizer(const BoardSpec& spec, std::size_t index) const;
    std::vector<BoardSample>    sampleStats() const;
    // Index of the board that fires the software trigger starting the chain
    // (board register id 0, else the first synchronised board), or -1 when no
    // board is configured for a synchronised start.
    int masterIndex() const;
    void writerLoop();          // single thread: drains writeQueue_ -> writer_
    void teardownWriter();      // close queue, join writer thread, close file

    std::string   outputDir_;
    std::uint32_t run_;
    Options       opt_;

    std::vector<std::unique_ptr<BoardRunner>> runners_;
    std::vector<std::string>                  names_;
    std::unique_ptr<StatsCollector>           stats_;

    // Unified file writer shared by every board.
    BlockQueue                     writeQueue_{8192}; // shared raw-buffer queue
    std::unique_ptr<RawFileWriter> writer_;
    std::thread                    writerThread_;
    std::atomic<bool>              writerFailed_{false};
    bool                           anyWrite_ = false; // any board contributes to the file

    bool prepared_ = false;
    bool running_  = false;
};

} // namespace caendaq
