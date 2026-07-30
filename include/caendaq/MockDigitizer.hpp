#pragma once
//
// MockDigitizer — a synthetic IDigitizer that fabricates plausible CAEN-style
// aggregate buffers at a configurable rate. It lets the full acquisition ->
// queue -> file pipeline (and its rotation / header logic) run and be verified
// without any hardware or the CAEN libraries.
//
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "caendaq/IDigitizer.hpp"

namespace caendaq {

class MockDigitizer : public IDigitizer {
public:
    struct Options {
        std::size_t   minBytes   = 4096;    // smallest fabricated buffer
        std::size_t   maxBytes   = 65536;   // largest fabricated buffer
        std::uint32_t ratePerSec = 200;     // buffers produced per second
        bool          waveforms  = false;   // emit a synthetic trace per event
        std::uint32_t traceSamples = 128;   // samples per trace when enabled (mult. of 16)
        std::uint32_t failEvery   = 0;      // set the board-FAIL bit ~1/failEvery aggregates (0 = never)
        // Firmware the mock emulates: PHA fills the energy spectrum, PSD fills
        // qshort/qlong. Chosen from the board's dpp so the online decoder routes
        // the same way it would for the real board. (x730 event framing is
        // structurally identical for the two, only the decode path differs.)
        DppType       dpp        = DppType::PSD;
        // Board id stamped into every aggregate header (top 5 bits of word[1])
        // and reported as boardRegId — must be unique per board so the unified
        // file/decoder can tell the boards apart.
        std::uint32_t boardId    = 0;
        // Start/Stop Mode the mock pretends to be configured with, so the
        // arm-then-trigger sequencing can be exercised without hardware.
        std::uint32_t startMode  = kStartModeSW;
    };

    MockDigitizer(BoardParams params, Options opt);
    explicit MockDigitizer(BoardParams params) : MockDigitizer(std::move(params), Options{}) {}

    bool open() override;
    bool configure() override;
    std::uint32_t startMode() const override { return opt_.startMode; }
    bool start() override;
    // The mock has no external start signal, so an armed board simply begins
    // producing data when the software trigger arrives (see sendSWTrigger).
    bool arm() override;
    bool sendSWTrigger() override;
    bool read(const char** data, std::size_t* size) override;
    // The mock keeps whatever is written to it, so online tuning can be driven
    // end to end without hardware: a write is accepted and reads back.
    bool writeRegister(std::uint32_t address, std::uint32_t value) override;
    bool readRegister(std::uint32_t address, std::uint32_t* value) override;
    bool stop() override;
    void close() override;

    BoardInfo info() const override { return info_; }
    const std::string& name() const override { return params_.name; }
    bool connected() const override { return open_; }

private:
    BoardParams  params_;
    Options      opt_;
    BoardInfo    info_;
    bool         open_    = false;
    bool         running_ = false;

    std::map<std::uint32_t, std::uint32_t> registers_;  // whatever online tuning wrote
    mutable std::mutex registersMutex_;

    std::vector<char> buffer_;                 // reusable readout buffer
    std::uint32_t     lfsr_ = 0x1234567u;      // deterministic pseudo-random state
    std::uint64_t     counter_ = 0;            // fabricated aggregate counter
    std::chrono::steady_clock::time_point nextEmit_{};

    std::uint32_t nextRand();
};

} // namespace caendaq
