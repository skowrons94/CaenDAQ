#pragma once
//
// HitTap — the hits of one board, for whoever builds coincidences from them.
//
// A coincidence needs every hit of every channel involved, in time, across
// boards; the spectra in HistogramStore have long forgotten when anything
// happened. The decode thread therefore appends a compact record per hit here
// (16 bytes), and the consumer (WebDAQ's coincidence builder) takes the whole
// batch every few hundred milliseconds.
//
// Off unless enabled, so a run without coincidences pays nothing. Bounded: past
// `capacity` hits the newest are dropped and counted rather than growing
// memory — a consumer that stopped taking cannot take the DAQ with it.
//
#include <cstdint>
#include <mutex>
#include <vector>

#include "caendaq/DecodedEvent.hpp"

namespace caendaq {

struct Hit {
    std::int64_t  timePs;   // DecodedEvent::timePs
    std::uint16_t energy;   // PHA energy, or PSD qlong
    std::uint8_t  channel;
    std::uint8_t  flags;    // bit0 pile-up, bit1 saturated, bit2 lost
    std::uint32_t reserved; // keeps the record 16 bytes, numpy-friendly
};
static_assert(sizeof(Hit) == 16, "Hit must stay 16 bytes: the Python dtype mirrors it");

class HitTap {
public:
    static constexpr std::size_t kDefaultCapacity = 1u << 20;   // ~16 MB

    void setEnabled(bool on) { std::lock_guard<std::mutex> lk(mtx_); enabled_ = on; if (!on) hits_.clear(); }
    bool enabled() const { std::lock_guard<std::mutex> lk(mtx_); return enabled_; }
    void setCapacity(std::size_t n) { std::lock_guard<std::mutex> lk(mtx_); capacity_ = n ? n : 1; }

    // Decode thread: append one block's worth at once (one lock per block).
    void append(const std::vector<Hit>& batch) {
        if (batch.empty()) return;
        std::lock_guard<std::mutex> lk(mtx_);
        if (!enabled_) return;
        const std::size_t room = capacity_ > hits_.size() ? capacity_ - hits_.size() : 0;
        const std::size_t n = batch.size() < room ? batch.size() : room;
        hits_.insert(hits_.end(), batch.begin(), batch.begin() + static_cast<long>(n));
        dropped_ += batch.size() - n;
    }

    // Consumer: everything since the last take, in decode order.
    std::vector<Hit> take() {
        std::vector<Hit> out;
        std::lock_guard<std::mutex> lk(mtx_);
        out.swap(hits_);
        return out;
    }

    std::uint64_t dropped() const { std::lock_guard<std::mutex> lk(mtx_); return dropped_; }

    static Hit fromEvent(const DecodedEvent& e) {
        Hit h{};
        h.timePs  = e.timePs;
        h.energy  = e.energy ? e.energy : e.qlong;
        h.channel = static_cast<std::uint8_t>(e.channel);
        h.flags   = static_cast<std::uint8_t>((e.pileup ? 1 : 0) | (e.satu ? 2 : 0) | (e.lost ? 4 : 0));
        return h;
    }

private:
    mutable std::mutex mtx_;
    bool               enabled_  = false;
    std::size_t        capacity_ = kDefaultCapacity;
    std::vector<Hit>   hits_;
    std::uint64_t      dropped_  = 0;
};

} // namespace caendaq
