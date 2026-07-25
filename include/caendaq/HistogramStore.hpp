#pragma once
//
// HistogramStore — thread-safe online spectra + latest waveform per channel.
//
// The decode thread calls fill() for every event; the consumer (later the
// pybind11 layer / a monitor) takes snapshots under the same lock. Histograms
// are plain integer-bin arrays — no ROOT — so the Python side can wrap them in a
// TH1 (or anything else) itself.
//
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "caendaq/DecodedEvent.hpp"

namespace caendaq {

class HistogramStore {
public:
    // Bin counts. Energy/qshort are 15-bit (0x8000), qlong is 16-bit.
    static constexpr std::size_t kEnergyBins = 32768;
    static constexpr std::size_t kQShortBins = 32768;
    static constexpr std::size_t kQLongBins  = 65536;
    // 2D DPP-PSD plot: x = qlong (charge), y = PSD ratio 1 - qshort/qlong in [0,1].
    static constexpr std::size_t kPsdXBins   = 2048;
    static constexpr std::size_t kPsdYBins   = 256;
    static constexpr std::uint32_t kQLongMax = 65536; // x-axis range

    // Cumulative per-channel counters, for rate computation downstream.
    struct Counts {
        std::uint64_t events = 0;
        std::uint64_t pileup = 0;
        std::uint64_t lost   = 0;
        std::uint64_t satu   = 0;
    };

    void fill(const DecodedEvent& e);

    // Snapshots (copied under the lock). Empty vector if the channel is unseen.
    std::vector<std::uint32_t> energy(std::uint16_t board, std::uint16_t ch) const;
    std::vector<std::uint32_t> qshort(std::uint16_t board, std::uint16_t ch) const;
    std::vector<std::uint32_t> qlong (std::uint16_t board, std::uint16_t ch) const;
    // Flattened 2D PSD histogram (row-major, kPsdXBins rows × kPsdYBins cols).
    // Empty until the channel receives a PSD event.
    std::vector<std::uint32_t> psd(std::uint16_t board, std::uint16_t ch) const;
    std::vector<std::int16_t>  waveform(std::uint16_t board, std::uint16_t ch) const;
    std::uint64_t              events(std::uint16_t board, std::uint16_t ch) const;
    Counts                     counts(std::uint16_t board, std::uint16_t ch) const;

    // (board,channel) pairs that have received at least one event.
    std::vector<std::pair<std::uint16_t, std::uint16_t>> channels() const;

    void reset();

private:
    struct Channel {
        std::vector<std::uint32_t> energy;
        std::vector<std::uint32_t> qshort;
        std::vector<std::uint32_t> qlong;
        std::vector<std::uint32_t> psd;      // flattened kPsdXBins × kPsdYBins
        std::vector<std::int16_t>  lastWave;
        Counts                     counts;
    };

    static std::uint32_t key(std::uint16_t board, std::uint16_t ch) {
        return (static_cast<std::uint32_t>(board) << 16) | ch;
    }
    const Channel* find(std::uint16_t board, std::uint16_t ch) const; // caller holds lock

    mutable std::mutex          mtx_;
    std::map<std::uint32_t, Channel> ch_;
};

} // namespace caendaq
