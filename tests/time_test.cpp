// Hit times as the coincidence builder sees them: the decoder places every
// event in picoseconds (fine time stamp included), unwraps the counter without
// being fooled by an event arriving out of order, and the time-between-hits
// histogram and the hit tap take it from there.
//
// V1730 DPP-PSD layout (vendored DataFrameBuilderPSD): Charge=bit30, TS=bit29,
// Extras=bit28, extras mode in bits[26:24]; event = [time][extras][charge].
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "caendaq/AggregateDecoder.hpp"
#include "caendaq/HistogramStore.hpp"
#include "caendaq/HitTap.hpp"

using namespace caendaq;

namespace {

// One board aggregate holding the given events on channel 0. extrasMode < 0
// leaves the extras word out (31-bit time stamps only).
std::vector<std::uint32_t> aggregate(const std::vector<std::uint32_t>& rawTS,
                                     const std::vector<std::uint32_t>& extras, int extrasMode) {
    const bool withExtras = extrasMode >= 0;
    const std::uint32_t evtSize = withExtras ? 3 : 2;
    std::uint32_t fmt = (1u << 30) | (1u << 29);
    if (withExtras) fmt |= (1u << 28) | (static_cast<std::uint32_t>(extrasMode) << 24);
    const std::uint32_t coupleSize = 2 + evtSize * static_cast<std::uint32_t>(rawTS.size());
    std::vector<std::uint32_t> b = {0xA0000000u | (4 + coupleSize), 0x1u, 0, 0, coupleSize, fmt};
    for (std::size_t i = 0; i < rawTS.size(); ++i) {
        b.push_back(rawTS[i] & 0x7FFFFFFFu);
        if (withExtras) b.push_back(extras[i]);
        b.push_back(100u | (200u << 16));
    }
    return b;
}

std::vector<DecodedEvent> decode(AggregateDecoder& dec, const std::vector<std::uint32_t>& b) {
    std::vector<DecodedEvent> out;
    dec.decode(reinterpret_cast<const char*>(b.data()), b.size() * 4,
               [&](const DecodedEvent& e) { out.push_back(e); });
    return out;
}

} // namespace

int main() {
    // 1. Extended + fine time stamp (extras mode 2), 2 ns ticks.
    {
        AggregateDecoder dec;
        dec.registerBoard(0, DppType::PSD, "V1730", 2000);
        const auto ev = decode(dec, aggregate({1000}, {(1u << 16) | 512u}, 2));
        assert(ev.size() == 1);
        const std::int64_t ticks = (1ll << 31) | 1000;
        printf("fine: timePs=%lld expected=%lld\n", (long long)ev[0].timePs,
               (long long)(ticks * 2000 + 1000));
        assert(ev[0].timePs == ticks * 2000 + 1000);   // 512/1024 of a 2 ns tick
    }
    // 2. A 31-bit counter wrapping, with a straggler from before the wrap.
    {
        AggregateDecoder dec;
        dec.registerBoard(0, DppType::PSD, "V1730", 2000);
        const std::uint64_t range = 1ull << 31;
        const auto ev = decode(dec, aggregate({0x7FFFFFF0u, 5u, 0x7FFFFFFBu, 10u}, {}, -1));
        assert(ev.size() == 4);
        const std::uint64_t want[4] = {0x7FFFFFF0u, range + 5, 0x7FFFFFFBu, range + 10};
        for (int i = 0; i < 4; ++i) {
            printf("wrap: event %d at %lld ticks (expected %llu)\n", i,
                   (long long)(ev[i].timePs / 2000), (unsigned long long)want[i]);
            assert(static_cast<std::uint64_t>(ev[i].timePs / 2000) == want[i]);
        }
    }
    // 2b. A channel silent for longer than half a wrap: channel 0 keeps the
    //     board's clock moving, channel 1 only fires again 0.7 wraps later.
    {
        AggregateDecoder dec;
        dec.registerBoard(0, DppType::PSD, "V1730", 2000);
        const std::uint64_t range = 1ull << 31;
        auto ev = decode(dec, aggregate({static_cast<std::uint32_t>(range / 10)}, {}, -1));   // ch0 at 0.1R
        assert(ev.size() == 1);
        // channel 1 (CH bit) first seen at 0.2R, then at 0.9R; channel 0 walks on past a wrap.
        std::vector<std::uint32_t> walk = {static_cast<std::uint32_t>(2 * range / 10) | 0x80000000u};
        for (int i = 3; i <= 12; ++i) walk.push_back(static_cast<std::uint32_t>((i * range / 10) % range));
        walk.push_back(static_cast<std::uint32_t>((13 * range / 10) % range) | 0x80000000u);   // ch1 at 1.3R
        std::vector<std::uint32_t> words = aggregate(walk, {}, -1);
        // aggregate() masks the CH bit away: put it back on the two channel-1 events.
        words[6] |= 0x80000000u;
        words[6 + 2 * 11] |= 0x80000000u;
        ev = decode(dec, words);
        const DecodedEvent& last = ev.back();
        printf("silent channel: ch%u at %.2f wraps (expected 1.30)\n", last.channel,
               static_cast<double>(last.timePs / 2000) / static_cast<double>(range));
        assert(last.channel == 1);
        assert(static_cast<std::uint64_t>(last.timePs / 2000) == 13 * range / 10);
    }
    // 3. No clock known: timePs stays 0, nothing else changes.
    {
        AggregateDecoder dec;
        dec.registerBoard(0, DppType::PSD, "Unknown", 0);
        const auto ev = decode(dec, aggregate({1234}, {}, -1));
        assert(ev.size() == 1 && ev[0].timePs == 0 && ev[0].timestamp == 1234);
    }
    // 4. Time between hits: 1 us apart lands at log10(1000 ns) * 100 = bin 300.
    {
        HistogramStore h;
        DecodedEvent e;
        e.channel = 3; e.energy = 10;
        e.timePs = 5'000'000;              h.fill(e);
        e.timePs = 5'000'000 + 1'000'000;  h.fill(e);
        e.timePs = 4'000'000;              h.fill(e);   // straggler: skipped
        const auto iv = h.interval(0, 3);
        assert(iv.size() == HistogramStore::kIntervalBins);
        std::uint64_t total = 0;
        for (auto c : iv) total += c;
        printf("interval: bin300=%u total=%llu\n", iv[300], (unsigned long long)total);
        assert(iv[300] == 1 && total == 1);
    }
    // 5. Hit tap: off by default, bounded when on.
    {
        HitTap t;
        DecodedEvent e; e.timePs = 42; e.qlong = 7; e.channel = 2; e.pileup = true;
        t.append({HitTap::fromEvent(e)});
        assert(t.take().empty());              // disabled: nothing kept
        t.setEnabled(true);
        t.setCapacity(3);
        t.append(std::vector<Hit>(5, HitTap::fromEvent(e)));
        const auto got = t.take();
        assert(got.size() == 3 && t.dropped() == 2);
        assert(got[0].timePs == 42 && got[0].energy == 7 && got[0].channel == 2 && got[0].flags == 1);
        assert(t.take().empty());
    }
    // 6. A V1725 PHA event made of the trace alone (format word 0x08000080, as a
    //    board with Board Configuration 0x10110 sends it): no time, no energy,
    //    the trace intact and the event counted as incomplete.
    {
        AggregateDecoder dec;
        dec.registerBoard(0, DppType::PHA, "V1725S", 4000);
        const std::uint32_t fmt = (1u << 27) | 1u;            // trace, 8 samples
        std::vector<std::uint32_t> b = {0xA0000000u | (4 + 2 + 4), 0x1u, 0, 0, 0x80000000u | 6, fmt};
        for (std::uint32_t i = 0; i < 4; ++i) b.push_back(((2 * i + 1001) << 16) | (2 * i + 1000));
        const auto ev = decode(dec, b);
        assert(ev.size() == 1);
        // wave.samples points into the decoder's buffer, still valid here.
        printf("trace only: energy=%u timePs=%lld samples=%u [%d..%d] incomplete=%llu\n",
               ev[0].energy, (long long)ev[0].timePs, ev[0].wave.length,
               ev[0].wave.samples[0], ev[0].wave.samples[7], (unsigned long long)dec.incompleteCount());
        assert(ev[0].energy == 0 && ev[0].timePs == 0 && ev[0].timestamp == 0);
        assert(ev[0].wave.length == 8 && ev[0].wave.samples[0] == 1000 && ev[0].wave.samples[7] == 1007);
        assert(dec.incompleteCount() == 1);
    }
    printf("PASS: hit times, unwrap, intervals and the hit tap.\n");
    return 0;
}
