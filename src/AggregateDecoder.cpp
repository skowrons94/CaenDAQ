#include "caendaq/AggregateDecoder.hpp"

#include "Utils.h" // vendored ExtractBits / BitRange

namespace caendaq {

namespace {
constexpr std::uint32_t kAggHeaderWords = 4;  // board-aggregate header size, in words
constexpr std::uint32_t kMaxChannels    = 16;

// Flag bits of the PHA "Extras" field of the energy word, matching RUReader.
// Bit positions per UM6769 rev.2, "Channel Aggregate Data Format for 724, 781
// and 782 series". Saturation used to be read from bit[0], which is DEAD_TIME:
// the V1724s here never raise it, so satuRate was flat zero while real
// saturated events (bit[4]) went unreported.
constexpr std::uint32_t kPhaDeadTimeBit  = 0x01; // bit[0] dead time before this event
constexpr std::uint32_t kPhaSatuBit      = 0x10; // bit[4] input saturation
constexpr std::uint32_t kPhaDoubleSatBit = 0x80; // bit[7] input stage AND trapezoid saturated
// bit[5] is a scaler, not a per-event flag: the board raises it once every N
// lost triggers, N = 1024/128/8192 per 0x1nA0[17:16]. lostRate therefore counts
// groups of N, not individual lost events — scale it before reading it as a rate.
constexpr std::uint32_t kPhaLostBit = 0x20; // bit[5] lost-trigger tag

constexpr std::uint32_t kPsdLostBit = 0x00001000; // PSD extra word (mode 1)
constexpr std::uint32_t kPsdOverBit = 0x00004000; // PSD extra word (mode 1): saturation

CAEN_DGTZ_DPPFirmware_t toCaenDpp(DppType t) {
    return (t == DppType::PSD) ? CAEN_DGTZ_DPPFirmware_PSD
                               : CAEN_DGTZ_DPPFirmware_PHA;
}
} // namespace

std::uint32_t AggregateDecoder::timeStampPs(const BoardInfo& bi) {
    if (bi.nsPerSample) return 1000u * bi.nsPerSample;
    const std::string& n = bi.modelName;
    if (n.find("724") != std::string::npos || n.find("781") != std::string::npos ||
        n.find("782") != std::string::npos) return 10000;
    if (n.find("725") != std::string::npos || n.find("720") != std::string::npos) return 4000;
    if (n.find("730") != std::string::npos) return 2000;
    return 0;
}

std::uint64_t AggregateDecoder::unwrap(ChannelClock& c, std::uint64_t raw, int width) {
    const std::uint64_t range = (width >= 64) ? 0 : (1ull << width);
    std::uint64_t full = raw;
    if (range != 0) {
        const std::uint64_t lowest = c.epoch > 0 ? c.epoch - 1 : 0;
        std::uint64_t best = c.epoch, bestDist = UINT64_MAX;
        for (std::uint64_t cand = lowest; cand <= c.epoch + 1 && c.seen; ++cand) {
            const std::uint64_t f = raw + cand * range;
            const std::uint64_t d = f > c.reference ? f - c.reference : c.reference - f;
            if (d < bestDist) { bestDist = d; best = cand; }
        }
        if (best > c.epoch) c.epoch = best;
        full = raw + best * range;
    }
    c.seen = true;
    if (full > c.reference) c.reference = full;
    return full;
}

void AggregateDecoder::setTime(DecodedEvent& e, std::uint32_t board, int width,
                               std::uint32_t fineTS, int fineBits) {
    BoardDec& bd = boards_[board];
    const std::uint64_t ticks = unwrap(bd.clock, e.timestamp, width);
    if (!bd.psPerTick) return;
    std::int64_t ps = static_cast<std::int64_t>(ticks * bd.psPerTick);
    if (fineBits > 0)
        ps += static_cast<std::int64_t>((static_cast<std::uint64_t>(fineTS) * bd.psPerTick) >> fineBits);
    e.timePs = ps;
}

void AggregateDecoder::registerBoard(std::uint32_t boardRegId, DppType dpp,
                                     const std::string& modelName, std::uint32_t psPerTick) {
    BoardDec bd;
    bd.psPerTick = psPerTick;
    bd.isPHA = (dpp != DppType::PSD);
    bd.frame = DataFrame(toCaenDpp(dpp), modelName);
    bd.frame.Build();
    bd.supported = bd.frame.IsSupported();
    boards_[boardRegId] = std::move(bd);
}

std::size_t AggregateDecoder::decode(const char* data, std::size_t nbytes,
                                     const EventFn& fn) {
    const std::uint32_t nwords = static_cast<std::uint32_t>(nbytes / sizeof(std::uint32_t));
    const auto* buf = reinterpret_cast<const std::uint32_t*>(data);

    std::size_t count = 0;
    const EventFn counting = [&](const DecodedEvent& e) { ++count; fn(e); };

    std::uint32_t offset = 0;
    while (offset + kAggHeaderWords <= nwords) {
        // A board aggregate always starts with 1010 in the four highest bits.
        if ((buf[offset] >> 28) != 0xA) { ++offset; continue; }

        const std::uint32_t aggLen = buf[offset] & 0x0FFFFFFF;
        if (aggLen < kAggHeaderWords) { ++offset; ++corrupt_; continue; }
        if (offset + aggLen > nwords) break; // partial aggregate: stop (writer keeps full data)

        const std::uint32_t board = (buf[offset + 1] & 0xF8000000u) >> 27;
        std::bitset<8> mask(buf[offset + 1] & 0xFF);
        if ((buf[offset + 1] >> 26) & 0x1u) ++boardFail_; // board-FAIL bit

        auto it = boards_.find(board);
        if (it != boards_.end() && it->second.supported) {
            if (it->second.isPHA) decodePHA(buf, board, mask, offset, aggLen, counting);
            else                  decodePSD(buf, board, mask, offset, aggLen, counting);
        }
        offset += aggLen;
    }
    return count;
}

std::uint32_t AggregateDecoder::unpackWave(const std::uint32_t* buf,
                                           const DataLayout& layout, std::uint32_t first) {
    const int nWords = layout.numSamples / 2; // two samples per 32-bit word
    if (nWords <= 0) return 0;

    const int lo = layout.fmtSample.first;
    const int hi = layout.fmtSample.second;

    if (layout.dualTrace) {
        wave_.resize(static_cast<std::size_t>(nWords));
        for (int i = 0; i < nWords; ++i)
            wave_[i] = static_cast<std::int16_t>(ExtractBits(buf[first + i], lo, hi));
        return static_cast<std::uint32_t>(nWords);
    }
    wave_.resize(static_cast<std::size_t>(2 * nWords));
    for (int i = 0; i < nWords; ++i) {
        const std::uint32_t w = buf[first + i];
        wave_[2 * i]     = static_cast<std::int16_t>(ExtractBits(w, lo,      hi));
        wave_[2 * i + 1] = static_cast<std::int16_t>(ExtractBits(w, lo + 16, hi + 16));
    }
    return static_cast<std::uint32_t>(2 * nWords);
}

void AggregateDecoder::decodePHA(const std::uint32_t* buf, std::uint32_t board,
                                 std::bitset<8> mask, std::uint32_t offset,
                                 std::uint32_t aggLen, const EventFn& fn) {
    DataFrame& frame = boards_[board].frame;

    std::uint8_t couples[8] = {0}; int nCouples = 0;
    for (int b = 0; b < 8; ++b) if (mask.test(b)) couples[nCouples++] = static_cast<std::uint8_t>(b);

    const std::uint32_t aggEnd = offset + aggLen;
    std::uint32_t pos = offset + kAggHeaderWords;

    for (int cpl = 0; cpl < nCouples; ++cpl) {
        if (pos + 2 > aggEnd) { ++corrupt_; return; }

        const std::uint32_t coupleSize = ExtractBits(buf[pos], frame.SizeFormat());
        frame.SetDataFormat(buf[pos + 1]);
        const DataLayout& layout = frame.Layout();

        if (coupleSize < 2 || pos + coupleSize > aggEnd || layout.evtSize == 0) { ++corrupt_; return; }

        const std::uint32_t nEvents = (coupleSize - 2) / layout.evtSize;
        const std::uint32_t base    = pos + 2;

        // A board that ships a channel aggregate whose payload it never wrote
        // returns a correct two-word header followed by random words. Random
        // words set bits the board never uses; one such event condemns the
        // whole aggregate, because the corruption has only ever been observed
        // all-or-nothing. Without this the online rates count noise as events
        // — a third of them in the run that exposed this. See DataLayout::resTS.
        if (layout.resTS || layout.resEnergy) {
            bool invalid = false;
            for (std::uint32_t evt = 0; evt < nEvents && !invalid; ++evt) {
                const std::uint32_t ev = base + evt * layout.evtSize;
                invalid = (buf[ev] & layout.resTS) != 0 ||
                          (buf[ev + layout.evtSize - 1] & layout.resEnergy) != 0;
            }
            if (invalid) {
                ++uninitialised_;
                uninitialisedEvents_ += nEvents;
                pos += coupleSize;
                continue;
            }
        }

        for (std::uint32_t evt = 0; evt < nEvents; ++evt) {
            const std::uint32_t ev = base + evt * layout.evtSize;
            // A board not recording time stamps or energies (Board Configuration
            // bits 18/19 off) sends the trace alone: no word may be read as one.
            const std::uint32_t timeWord   = layout.hasTS ? buf[ev] : 0;
            const std::uint32_t energyWord = layout.hasEnergy ? buf[ev + layout.evtSize - 1] : 0;
            const std::uint32_t lastWords  = layout.hasEnergy ? 1 : 0;   // words after the extras
            if (!layout.hasTS || !layout.hasEnergy) ++incomplete_;

            const std::uint64_t rawTS = ExtractBits(timeWord, layout.fmtTS);

            std::uint32_t channel = couples[cpl];
            if (layout.hasChannelBit) channel = ((timeWord >> 31) & 0x1) + couples[cpl] * 2;
            if (channel >= kMaxChannels) continue;

            std::uint32_t extendedTS = 0; int extendedBits = 0;
            std::uint32_t fineTS = 0; int fineBits = 0;
            if (layout.hasExtras) {
                const std::uint32_t extras2 = buf[ev + layout.evtSize - 1 - lastWords];
                if (layout.extrasMode <= 2) { extendedTS = extras2 >> 16; extendedBits = 16; }
                if (layout.extrasMode == 2) { fineTS = extras2 & 0x3FF; fineBits = 10; }
            }

            // Quality flags from the PHA extras field of the energy word.
            const std::uint32_t flags = ExtractBits(energyWord, layout.fmtExtras);

            DecodedEvent e;
            e.board     = static_cast<std::uint16_t>(board);
            e.channel   = static_cast<std::uint16_t>(channel);
            e.energy    = static_cast<std::uint16_t>(energyWord & 0x7FFF);
            e.pileup    = (energyWord >> 15) & 0x1;
            e.satu      = (flags & (kPhaSatuBit | kPhaDoubleSatBit)) != 0;
            e.lost      = (flags & kPhaLostBit) != 0;
            e.timestamp = extendedBits ? ((static_cast<std::uint64_t>(extendedTS) << layout.tsBits) | rawTS)
                                       : rawTS;
            if (layout.hasTS) setTime(e, board, layout.tsBits + extendedBits, fineTS, fineBits);
            if (layout.hasTrace) { std::uint32_t n = unpackWave(buf, layout, ev + (layout.hasTS ? 1 : 0)); e.wave = {wave_.data(), n}; }
            fn(e);
        }
        pos += coupleSize;
    }
}

void AggregateDecoder::decodePSD(const std::uint32_t* buf, std::uint32_t board,
                                 std::bitset<8> mask, std::uint32_t offset,
                                 std::uint32_t aggLen, const EventFn& fn) {
    DataFrame& frame = boards_[board].frame;

    std::uint8_t couples[8] = {0}; int nCouples = 0;
    for (int b = 0; b < 8; ++b) if (mask.test(b)) couples[nCouples++] = static_cast<std::uint8_t>(b);

    const std::uint32_t aggEnd = offset + aggLen;
    std::uint32_t pos = offset + kAggHeaderWords;

    for (int cpl = 0; cpl < nCouples; ++cpl) {
        if (pos + 2 > aggEnd) { ++corrupt_; return; }

        const std::uint32_t coupleSize = ExtractBits(buf[pos], frame.SizeFormat());
        frame.SetDataFormat(buf[pos + 1]);
        const DataLayout& layout = frame.Layout();

        if (coupleSize < 2 || pos + coupleSize > aggEnd || layout.evtSize == 0) { ++corrupt_; return; }

        const std::uint32_t nEvents = (coupleSize - 2) / layout.evtSize;
        const std::uint32_t base    = pos + 2;

        for (std::uint32_t evt = 0; evt < nEvents; ++evt) {
            const std::uint32_t ev = base + evt * layout.evtSize;
            const std::uint32_t timeWord   = layout.hasTS ? buf[ev] : 0;
            const std::uint32_t chargeWord = layout.hasCharge ? buf[ev + layout.evtSize - 1] : 0;
            const std::uint32_t lastWords  = layout.hasCharge ? 1 : 0;
            if (!layout.hasTS || !layout.hasCharge) ++incomplete_;

            const std::uint64_t rawTS = ExtractBits(timeWord, layout.fmtTS);

            std::uint32_t channel = couples[cpl];
            if (layout.hasChannelBit) channel = ((timeWord >> 31) & 0x1) + couples[cpl] * 2;
            if (channel >= kMaxChannels) continue;

            std::uint32_t extendedTS = 0; int extendedBits = 0;
            std::uint32_t fineTS = 0; int fineBits = 0;
            bool satu = false, lost = false;
            if (layout.hasExtras) {
                const std::uint32_t extras2 = buf[ev + layout.evtSize - 1 - lastWords];
                if (layout.extrasConfig) {
                    if (layout.extrasMode <= 2) { extendedTS = extras2 >> 16; extendedBits = 16; }
                    if (layout.extrasMode == 2) { fineTS = extras2 & 0x3FF; fineBits = 10; }
                    if (layout.extrasMode == 1) {
                        satu = (extras2 & kPsdOverBit) != 0;
                        lost = (extras2 & kPsdLostBit) != 0;
                    }
                } else {
                    extendedTS = extras2 & 0x7FFF; extendedBits = 15; // x720
                }
            }

            DecodedEvent e;
            e.board     = static_cast<std::uint16_t>(board);
            e.channel   = static_cast<std::uint16_t>(channel);
            e.qshort    = static_cast<std::uint16_t>(chargeWord & 0x7FFF);
            e.qlong     = static_cast<std::uint16_t>(chargeWord >> 16);
            e.pileup    = (chargeWord >> 15) & 0x1;
            e.satu      = satu;
            e.lost      = lost;
            e.timestamp = extendedBits ? ((static_cast<std::uint64_t>(extendedTS) << layout.tsBits) | rawTS)
                                       : rawTS;
            if (layout.hasTS) setTime(e, board, layout.tsBits + extendedBits, fineTS, fineBits);
            if (layout.hasTrace) { std::uint32_t n = unpackWave(buf, layout, ev + (layout.hasTS ? 1 : 0)); e.wave = {wave_.data(), n}; }
            fn(e);
        }
        pos += coupleSize;
    }
}

} // namespace caendaq
