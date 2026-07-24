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
    info_.boardRegId       = 0;
    info_.dppType          = DppType::PSD;
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

    // Fabricate ONE valid V1730 DPP-PSD board aggregate so the whole chain
    // (RUReader-readable file + online decoder) sees real events. Layout matches
    // the vendored decoder: Charge + TS enabled, no extras/trace.
    //   couple 0 -> channels 0/1 (via the CH bit), qshort/qlong ~ a soft peak.
    const std::uint32_t nEvents = 16 + (nextRand() % 48); // 16..63 events
    const std::uint32_t coupleSize = 2 + nEvents * 2;     // hdr(2) + evtSize(2) each
    const std::uint32_t aggLen     = 4 + coupleSize;      // 4-word board-agg header

    auto* w = reinterpret_cast<std::uint32_t*>(buffer_.data());
    std::size_t k = 0;
    w[k++] = 0xA0000000u | (aggLen & 0x0FFFFFFFu);   // sync + length
    w[k++] = (0u << 27) | 0x01u;                     // board 0, channelMask = couple 0
    w[k++] = static_cast<std::uint32_t>(counter_ & 0x7FFFFF); // aggregate counter
    w[k++] = static_cast<std::uint32_t>(counter_);            // aggregate time tag
    w[k++] = coupleSize;                             // couple-aggregate size
    w[k++] = (1u << 30) | (1u << 29);               // data format: Charge + TS

    for (std::uint32_t e = 0; e < nEvents; ++e) {
        const std::uint32_t chBit = e & 1u;          // alternate channel 0/1
        const std::uint32_t ts    = static_cast<std::uint32_t>(counter_ * 1000 + e) & 0x7FFFFFFFu;
        // qlong ~ soft peak near 6000 (sum of uniforms ≈ Gaussian); qshort ≈ qlong/2.
        const std::uint32_t noise = (nextRand() % 800) + (nextRand() % 800) + (nextRand() % 800);
        const std::uint32_t qlong  = (4800u + noise) & 0xFFFFu;
        const std::uint32_t qshort = (qlong / 2) & 0x7FFFu;
        const std::uint32_t pileup = (nextRand() % 20u == 0u) ? 1u : 0u; // ~5% pile-up
        w[k++] = (chBit << 31) | (ts & 0x7FFFFFFFu);              // time word (+CH bit)
        w[k++] = qshort | (pileup << 15) | (qlong << 16);         // charge word (+PU bit)
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
