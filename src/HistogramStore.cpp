#include "caendaq/HistogramStore.hpp"

namespace caendaq {

void HistogramStore::fill(const DecodedEvent& e) {
    std::lock_guard<std::mutex> lk(mtx_);
    Channel& c = ch_[key(e.board, e.channel)];

    if (e.energy) {
        if (c.energy.empty()) c.energy.assign(kEnergyBins, 0);
        if (e.energy < kEnergyBins) ++c.energy[e.energy];
    }
    if (e.qshort || e.qlong) {
        if (c.qshort.empty()) c.qshort.assign(kQShortBins, 0);
        if (c.qlong.empty())  c.qlong.assign(kQLongBins, 0);
        if (e.qshort < kQShortBins) ++c.qshort[e.qshort];
        ++c.qlong[e.qlong]; // qlong is 16-bit, always within kQLongBins

        // 2D DPP-PSD histogram: x = qlong (charge), y = PSD ratio 1 - qs/ql.
        if (e.qlong > 0) {
            if (c.psd.empty()) c.psd.assign(kPsdXBins * kPsdYBins, 0);
            std::size_t xb = (static_cast<std::uint64_t>(e.qlong) * kPsdXBins) / kQLongMax;
            if (xb >= kPsdXBins) xb = kPsdXBins - 1;
            double ratio = 1.0 - static_cast<double>(e.qshort) / static_cast<double>(e.qlong);
            if (ratio < 0.0) ratio = 0.0;
            if (ratio > 1.0) ratio = 1.0;
            std::size_t yb = static_cast<std::size_t>(ratio * kPsdYBins);
            if (yb >= kPsdYBins) yb = kPsdYBins - 1;
            ++c.psd[xb * kPsdYBins + yb];
        }
    }
    if (e.wave.samples && e.wave.length) {
        c.lastWave.assign(e.wave.samples, e.wave.samples + e.wave.length);
    }
    ++c.counts.events;
    if (e.pileup) ++c.counts.pileup;
    if (e.lost)   ++c.counts.lost;
    if (e.satu)   ++c.counts.satu;
}

const HistogramStore::Channel* HistogramStore::find(std::uint16_t board, std::uint16_t ch) const {
    auto it = ch_.find(key(board, ch));
    return it == ch_.end() ? nullptr : &it->second;
}

std::vector<std::uint32_t> HistogramStore::energy(std::uint16_t board, std::uint16_t ch) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Channel* c = find(board, ch);
    return c ? c->energy : std::vector<std::uint32_t>{};
}

std::vector<std::uint32_t> HistogramStore::qshort(std::uint16_t board, std::uint16_t ch) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Channel* c = find(board, ch);
    return c ? c->qshort : std::vector<std::uint32_t>{};
}

std::vector<std::uint32_t> HistogramStore::qlong(std::uint16_t board, std::uint16_t ch) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Channel* c = find(board, ch);
    return c ? c->qlong : std::vector<std::uint32_t>{};
}

std::vector<std::uint32_t> HistogramStore::psd(std::uint16_t board, std::uint16_t ch) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Channel* c = find(board, ch);
    return c ? c->psd : std::vector<std::uint32_t>{};
}

std::vector<std::int16_t> HistogramStore::waveform(std::uint16_t board, std::uint16_t ch) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Channel* c = find(board, ch);
    return c ? c->lastWave : std::vector<std::int16_t>{};
}

std::uint64_t HistogramStore::events(std::uint16_t board, std::uint16_t ch) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Channel* c = find(board, ch);
    return c ? c->counts.events : 0;
}

HistogramStore::Counts HistogramStore::counts(std::uint16_t board, std::uint16_t ch) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Channel* c = find(board, ch);
    return c ? c->counts : Counts{};
}

std::vector<std::pair<std::uint16_t, std::uint16_t>> HistogramStore::channels() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::pair<std::uint16_t, std::uint16_t>> out;
    out.reserve(ch_.size());
    for (const auto& kv : ch_)
        out.emplace_back(static_cast<std::uint16_t>(kv.first >> 16),
                         static_cast<std::uint16_t>(kv.first & 0xFFFF));
    return out;
}

void HistogramStore::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    ch_.clear();
}

} // namespace caendaq
