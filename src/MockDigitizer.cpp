#include "caendaq/MockDigitizer.hpp"

#include <thread>

#include "caendaq/Log.hpp"

namespace caendaq {

MockDigitizer::MockDigitizer(BoardParams params, Options opt)
    : params_(std::move(params)), opt_(opt) {
    if (opt_.maxBytes < opt_.minBytes) opt_.maxBytes = opt_.minBytes;
    // Always big enough for one fabricated aggregate (6 hdr + 2*63 event words).
    buffer_.resize(std::max<std::size_t>(opt_.maxBytes, 4096));

    // Plausible synthetic identity so the file header is exercised end-to-end.
    info_.modelName        = "MOCK-V1730";
    info_.model            = 730;
    info_.familyCode       = 0;
    info_.channels         = 16;
    info_.adcNBits         = 14;
    info_.serialNumber     = 4818;
    info_.boardRegId       = opt_.boardId & 0x1Fu;   // unique per board in the run
    info_.dppType          = opt_.dpp;               // PHA or PSD, from the board's config
    info_.nsPerSample      = 2;
    info_.nsPerTimetag     = 8;
    info_.rocFirmware      = "mock-roc";
    info_.amcFirmware      = "mock-amc";
    info_.channelEnableMask = 0xFFFF;
    info_.connType         = params_.connType;
    info_.linkNum          = params_.linkNum;
    info_.node             = params_.node;
    info_.vmeBase          = params_.vmeBase;
}

std::uint32_t MockDigitizer::nextRand() {
    // xorshift32 — deterministic, no <random> dependency, good enough for filler.
    lfsr_ ^= lfsr_ << 13;
    lfsr_ ^= lfsr_ >> 17;
    lfsr_ ^= lfsr_ << 5;
    return lfsr_;
}

bool MockDigitizer::open() {
    open_ = true;
    LOG_INFO("MockDigitizer[" << params_.name << "]: open");
    return true;
}

bool MockDigitizer::configure() {
    LOG_INFO("MockDigitizer[" << params_.name << "]: configure ("
             << (params_.configPath.empty() ? "no file" : params_.configPath) << ")");
    return true;
}

bool MockDigitizer::start() {
    running_ = true;
    counter_ = 0;
    nextEmit_ = std::chrono::steady_clock::now();
    LOG_INFO("MockDigitizer[" << params_.name << "]: start");
    return true;
}

bool MockDigitizer::read(const char** data, std::size_t* size) {
    *data = nullptr;
    *size = 0;
    if (!running_) return true;

    // Pace emission to the requested rate; return an empty (but valid) read when
    // it isn't time yet, exactly like a real board with no pending data.
    const auto now = std::chrono::steady_clock::now();
    if (now < nextEmit_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    }
    const auto period = std::chrono::nanoseconds(1'000'000'000ull /
                        (opt_.ratePerSec ? opt_.ratePerSec : 1));
    nextEmit_ += period;

    // Fabricate ONE valid V1730 (x730 family) board aggregate so the whole chain
    // (RUReader-readable file + online decoder) sees real events. The x730 event
    // framing is identical for DPP-PHA and DPP-PSD (TS word + one data word);
    // only the meaning of the data word and the decode path differ, so the same
    // aggregate serves both — see the per-event word below. When waveforms are
    // enabled the events also carry a synthetic trace (Trace flag + samples),
    // exactly as a real board configured for waveform recording would.
    //   couple 0 -> channels 0/1 (via the CH bit); energy | qshort/qlong ~ a soft peak.
    const bool          trace     = opt_.waveforms && opt_.traceSamples >= 2;
    const std::uint32_t nSamp     = trace ? (opt_.traceSamples & ~0xFu) : 0u; // multiple of 16
    const std::uint32_t nTraceW   = nSamp / 2;              // 2 samples / word
    const std::uint32_t evtSize   = 2 + nTraceW;            // TS + [trace] + charge
    const std::uint32_t nEvents   = 8 + (nextRand() % 24);  // 8..31 events
    const std::uint32_t coupleSize = 2 + nEvents * evtSize; // fmt(2) + events
    const std::uint32_t aggLen     = 4 + coupleSize;        // 4-word board-agg header

    // Grow the scratch buffer if this aggregate needs more room.
    if (buffer_.size() < static_cast<std::size_t>(aggLen) * sizeof(std::uint32_t))
        buffer_.resize(static_cast<std::size_t>(aggLen) * sizeof(std::uint32_t));

    auto* w = reinterpret_cast<std::uint32_t*>(buffer_.data());
    std::size_t k = 0;
    const std::uint32_t fail =
        (opt_.failEvery && (nextRand() % opt_.failEvery == 0u)) ? 1u : 0u; // board-FAIL (off by default)
    w[k++] = 0xA0000000u | (aggLen & 0x0FFFFFFFu);   // sync + length
    w[k++] = ((info_.boardRegId & 0x1Fu) << 27) | (fail << 26) | 0x01u; // board id, [FAIL], channelMask = couple 0
    w[k++] = static_cast<std::uint32_t>(counter_ & 0x7FFFFF); // aggregate counter
    w[k++] = static_cast<std::uint32_t>(counter_);            // aggregate time tag
    w[k++] = coupleSize;                             // couple-aggregate size
    // data format: Charge(30) + TS(29) [+ Trace(27) + NumSamples cfg in bits 0..15]
    std::uint32_t fmt = (1u << 30) | (1u << 29);
    if (trace) fmt |= (1u << 27) | ((nSamp / 8u) & 0xFFFFu); // NumSamples cfg = samples/8
    w[k++] = fmt;

    for (std::uint32_t e = 0; e < nEvents; ++e) {
        const std::uint32_t chBit = e & 1u;          // alternate channel 0/1
        const std::uint32_t ts    = static_cast<std::uint32_t>(counter_ * 1000 + e) & 0x7FFFFFFFu;
        w[k++] = (chBit << 31) | (ts & 0x7FFFFFFFu);              // time word (+CH bit)

        if (trace) {
            // Synthetic pulse: flat baseline, quick rise, linear decay (14-bit).
            for (std::uint32_t s = 0; s < nSamp; s += 2) {
                auto sample = [&](std::uint32_t idx) -> std::uint32_t {
                    int v = 1000; // baseline
                    if (idx >= 20 && idx < 30)      v = 1000 + (idx - 20) * 250;      // rise
                    else if (idx >= 30)             v = 3500 - static_cast<int>((idx - 30) * 30); // decay
                    if (v < 300)   v = 300;
                    if (v > 16383) v = 16383;
                    return static_cast<std::uint32_t>(v);
                };
                w[k++] = (sample(s) & 0x3FFFu) | ((sample(s + 1) & 0x3FFFu) << 16);
            }
        }

        const std::uint32_t pileup = (nextRand() % 20u == 0u) ? 1u : 0u; // ~5% pile-up
        if (info_.dppType == DppType::PHA) {
            // PHA energy word: 15-bit energy peak (bits 0..14) + pile-up (bit 15).
            // The PHA extras/flag field (bits 16..20, read as satu/lost) stays 0.
            const std::uint32_t noise  = (nextRand() % 1600) + (nextRand() % 1600);
            const std::uint32_t energy = (8000u + noise) & 0x7FFFu; // soft peak ~8000-11000
            w[k++] = energy | (pileup << 15);                       // energy word (+PU bit)
        } else {
            // PSD charge word: qshort (bits 0..14) + pile-up (bit 15) + qlong (bits 16..31).
            // qlong ~ soft peak near 6000 (sum of uniforms ≈ Gaussian); qshort ≈ qlong/2.
            const std::uint32_t noise = (nextRand() % 800) + (nextRand() % 800) + (nextRand() % 800);
            const std::uint32_t qlong  = (4800u + noise) & 0xFFFFu;
            const std::uint32_t qshort = (qlong / 2) & 0x7FFFu;
            w[k++] = qshort | (pileup << 15) | (qlong << 16);       // charge word (+PU bit)
        }
    }
    ++counter_;

    *data = buffer_.data();
    *size = k * sizeof(std::uint32_t);
    return true;
}

bool MockDigitizer::stop() {
    running_ = false;
    LOG_INFO("MockDigitizer[" << params_.name << "]: stop (" << counter_ << " buffers emitted)");
    return true;
}

void MockDigitizer::close() {
    if (!open_) return;
    open_ = false;
    LOG_INFO("MockDigitizer[" << params_.name << "]: close");
}

} // namespace caendaq
