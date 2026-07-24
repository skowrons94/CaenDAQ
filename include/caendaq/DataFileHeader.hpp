#pragma once
//
// On-disk framing for .caendat files — the exact XDAQ file header, so the
// existing RUReader reads CaenDAQ output with NO changes.
//
// Source of truth: rubuilder/rucaendgtz/ReadoutUnit.{h,cc}
//   struct fileHeader_t {
//     uint32_t hSize;                 // = 6 + nbBoards (total header size in words)
//     uint32_t keyFrame = 0xFA0201A0; // sync/magic key
//     uint32_t nbBoards;
//     uint32_t ts_0 = 0;              // AGAVA timestamp low  (unused here -> 0)
//     uint32_t ts_1 = 0;              // AGAVA timestamp high (unused here -> 0)
//     uint32_t epochStartTime;        // time(nullptr) as uint32
//     uint32_t boardDef[nbBoards];    // one packed word per board:
//        bits[5:0]  = nsPerSample     bits[11:6] = nsPerTimetag
//        bits[15:12]= dppVersion      bits[23:16]= channels
//        bits[31:24]= boardRegId      (dppVersion: 0=PHA, 1=PSD — RUReader supported set)
//   };
// Written once, to the first file of a run. Split/continuation files start
// directly with a board aggregate (RUReader detects this via the 0xA marker).
// There is NO trailer — RUReader reads aggregates until EOF, so nothing must
// follow the payload.
//
// The words are little-endian (x86, as XDAQ/RUReader assume).
//
#include <cstdint>
#include <ctime>
#include <vector>

#include "caendaq/BoardInfo.hpp"

namespace caendaq {

constexpr std::uint32_t kXdaqKeyFrame = 0xFA0201A0u;

// Per-board fields packed into one boardDef word.
struct XdaqBoardDef {
    std::uint8_t nsPerSample  = 0;
    std::uint8_t nsPerTimetag = 0;
    std::uint8_t dppVersion   = 0; // CAEN firmware enum value: 0=PHA, 1=PSD
    std::uint8_t channels     = 0;
    std::uint8_t boardRegId   = 0;
};

// Map our stable DppType to the XDAQ header's dppVersion nibble (RUReader only
// understands 0=PHA and 1=PSD).
inline std::uint8_t xdaqDppVersion(DppType t) {
    switch (t) {
        case DppType::PSD: return 1;
        case DppType::PHA: return 0;
        default:           return 0; // best effort; other firmwares aren't RUReader-decodable
    }
}

inline XdaqBoardDef boardDefFrom(const BoardInfo& bi) {
    XdaqBoardDef d;
    d.nsPerSample  = bi.nsPerSample;
    d.nsPerTimetag = bi.nsPerTimetag;
    d.dppVersion   = xdaqDppVersion(bi.dppType);
    d.channels     = static_cast<std::uint8_t>(bi.channels);
    d.boardRegId   = static_cast<std::uint8_t>(bi.boardRegId);
    return d;
}

inline std::uint32_t packBoardDef(const XdaqBoardDef& b) {
    return (static_cast<std::uint32_t>(b.nsPerSample)  & 0x3Fu)
         | (static_cast<std::uint32_t>(b.nsPerTimetag) & 0x3Fu) << 6
         | (static_cast<std::uint32_t>(b.dppVersion)   & 0x0Fu) << 12
         | (static_cast<std::uint32_t>(b.channels)     & 0xFFu) << 16
         | (static_cast<std::uint32_t>(b.boardRegId)   & 0xFFu) << 24;
}

// Serialize the XDAQ file header as a little-endian uint32 word array.
inline std::vector<std::uint32_t>
buildXdaqHeader(const std::vector<XdaqBoardDef>& boards,
                std::uint32_t epochStartTime,
                std::uint32_t ts0 = 0, std::uint32_t ts1 = 0) {
    const std::uint32_t nb = static_cast<std::uint32_t>(boards.size());
    std::vector<std::uint32_t> h;
    h.reserve(6 + nb);
    h.push_back(6u + nb);        // [0] hSize
    h.push_back(kXdaqKeyFrame);  // [1] keyFrame
    h.push_back(nb);             // [2] nbBoards
    h.push_back(ts0);            // [3] ts_0
    h.push_back(ts1);            // [4] ts_1
    h.push_back(epochStartTime); // [5] epochStartTime
    for (const auto& b : boards) h.push_back(packBoardDef(b));
    return h;
}

inline std::uint32_t nowEpoch32() {
    return static_cast<std::uint32_t>(std::time(nullptr));
}

} // namespace caendaq
