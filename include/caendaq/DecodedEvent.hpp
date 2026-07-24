#pragma once
//
// DecodedEvent — one physics event extracted from a raw CAEN aggregate.
//
// This is the online view: energy/charge + channel (+ optional waveform), which
// is what spectra, PSD plots and rates need. It deliberately omits the full
// offline 64-bit time-stamp wrap reconstruction that RUReader does for list-mode
// output — the raw time stamp here is enough for online rate monitoring.
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
    std::uint16_t energy   = 0;    // DPP-PHA
    std::uint16_t qshort   = 0;    // DPP-PSD
    std::uint16_t qlong    = 0;    // DPP-PSD
    bool          pileup   = false;
    bool          lost     = false;
    bool          satu     = false;
    WaveView      wave;            // wave.samples == nullptr when the event has no trace
};

} // namespace caendaq
