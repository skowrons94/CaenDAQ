#pragma once
//
// DecodedEvent — one physics event extracted from a raw CAEN aggregate.
//
// This is the online view: energy/charge + channel (+ optional waveform), which
// is what spectra, PSD plots, rates and online coincidences need. `timePs` is
// the hit time on the board's clock, unwrapped and with the fine time stamp
// when the board records it, as RUReader computes it for list-mode output.
//
#include <cstdint>

namespace caendaq {

struct WaveView {
    const std::int16_t* samples = nullptr; // decoder-owned, valid only during the callback
    std::uint32_t       length  = 0;
};

struct DecodedEvent {
    std::uint16_t board    = 0;
    std::uint16_t channel  = 0;
    std::uint64_t timestamp = 0;   // raw (+extended) time stamp, no wrap correction
    std::int64_t  timePs   = 0;    // unwrapped time in ps (+ fine time stamp), 0 if the clock is unknown
    std::uint16_t energy   = 0;    // DPP-PHA
    std::uint16_t qshort   = 0;    // DPP-PSD
    std::uint16_t qlong    = 0;    // DPP-PSD
    bool          pileup   = false;
    bool          lost     = false;
    bool          satu     = false;
    WaveView      wave;            // wave.samples == nullptr when the event has no trace
};

} // namespace caendaq
