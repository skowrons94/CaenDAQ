#pragma once
//
// MockDigitizer — a synthetic IDigitizer that fabricates plausible CAEN-style
// aggregate buffers at a configurable rate. It lets the full acquisition ->
// queue -> file pipeline (and its rotation / header logic) run and be verified
// without any hardware or the CAEN libraries.
//
#include <chrono>
#include <cstdint>
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
    };

    MockDigitizer(BoardParams params, Options opt);
    explicit MockDigitizer(BoardParams params) : MockDigitizer(std::move(params), Options{}) {}

    bool open() override;
    bool configure() override;
    bool start() override;
    bool read(const char** data, std::size_t* size) override;
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

    std::vector<char> buffer_;                 // reusable readout buffer
    std::uint32_t     lfsr_ = 0x1234567u;      // deterministic pseudo-random state
    std::uint64_t     counter_ = 0;            // fabricated aggregate counter
    std::chrono::steady_clock::time_point nextEmit_{};

    std::uint32_t nextRand();
};

} // namespace caendaq
