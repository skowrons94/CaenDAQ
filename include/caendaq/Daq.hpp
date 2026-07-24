#pragma once
//
// Daq — top-level orchestrator over one or more boards. Each board runs its own
// reader/writer(/decode) pipeline (BoardRunner) and writes its own .caendat.
// This is the object the Python (pybind11) layer wraps as an instance.
//
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "caendaq/BoardRunner.hpp"
#include "caendaq/HistogramStore.hpp"
#include "caendaq/IDigitizer.hpp"
#include "caendaq/MockDigitizer.hpp"
#include "caendaq/StatsCollector.hpp"

namespace caendaq {

struct BoardSpec {
    BoardParams   params;                 // name, connType, link, node, vmeBase, configPath
    bool          mock       = false;      // use MockDigitizer instead of a real board
    bool          decode     = false;      // enable the parallel decode tap
    bool          write      = true;       // write .caendat files (false = decode-only)
    std::uint32_t mockRate   = 200;        // mock buffers/s (mock only)
};

class Daq {
public:
    struct Options {
        std::uint64_t maxFileBytes  = 0;
        bool          writeHeader   = true;
        std::string   graphiteHost;          // empty = no Graphite push
        int           graphitePort  = 2003;
        int           statsIntervalMs = 1000;
    };

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

    // Per-board live counters.
    std::uint64_t buffersRead(int board) const;
    std::uint64_t bytesRead(int board) const;
    std::uint64_t bytesWritten(int board) const;
    std::uint64_t blocksDropped(int board) const;
    std::uint64_t commErrors(int board) const;
    std::uint64_t eventsDecoded(int board) const;

    // Latest per-board / per-channel rates (events/pileup/lost/satu per second,
    // and file write rate). Empty until a run with decode has been started.
    std::vector<BoardRate> stats() const;

private:
    std::unique_ptr<IDigitizer> makeDigitizer(const BoardSpec& spec) const;
    std::vector<BoardSample>    sampleStats() const;

    std::string   outputDir_;
    std::uint32_t run_;
    Options       opt_;

    std::vector<std::unique_ptr<BoardRunner>> runners_;
    std::vector<std::string>                  names_;
    std::unique_ptr<StatsCollector>           stats_;
    bool prepared_ = false;
    bool running_  = false;
};

} // namespace caendaq
