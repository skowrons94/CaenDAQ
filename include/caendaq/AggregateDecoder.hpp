#pragma once
//
// AggregateDecoder — walks a raw CAEN readout buffer (the exact bytes ReadData
// returns / the .caendat payload) and emits DecodedEvents.
//
// The board-specific bit layouts come from RUReader's vendored format classes
// (vendor/rureader/), so this stays faithful to the proven offline decoder; the
// walk/resync logic mirrors RUReader::Convert. Nothing here throws or allocates
// on the hot path, and every buffer access is bounds-checked — a corrupt buffer
// is skipped, never fatal.
//
#include <bitset>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "caendaq/BoardInfo.hpp"
#include "caendaq/DecodedEvent.hpp"

#include "DataFrame.h" // vendored RUReader decoder

namespace caendaq {

class AggregateDecoder {
public:
    using EventFn = std::function<void(const DecodedEvent&)>;

    // Register a board (by its aggregate board id) with its firmware + model
    // name, so the matching bit tables are built. Safe to call once per board.
    void registerBoard(std::uint32_t boardRegId, DppType dpp, const std::string& modelName);

    // Convenience: register straight from a BoardInfo.
    void registerBoard(const BoardInfo& bi) {
        registerBoard(bi.boardRegId, bi.dppType, bi.modelName);
    }

    // Decode a raw buffer (bytes). Calls `fn` for every event. Returns the number
    // of events emitted. Never throws.
    std::size_t decode(const char* data, std::size_t nbytes, const EventFn& fn);

private:
    void decodePHA(const std::uint32_t* buf, std::uint32_t board, std::bitset<8> mask,
                   std::uint32_t offset, std::uint32_t aggLen, const EventFn& fn);
    void decodePSD(const std::uint32_t* buf, std::uint32_t board, std::bitset<8> mask,
                   std::uint32_t offset, std::uint32_t aggLen, const EventFn& fn);
    // Fills wave_ with trace 1 and returns the sample count (0 if none).
    std::uint32_t unpackWave(const std::uint32_t* buf, const DataLayout& layout,
                             std::uint32_t ev);

    struct BoardDec {
        DataFrame frame;
        bool      isPHA = false;
        bool      supported = false;
    };

    std::map<std::uint32_t, BoardDec> boards_;
    std::vector<std::int16_t>         wave_;   // reusable trace buffer
    std::uint64_t                     corrupt_ = 0; // count of skipped/corrupt aggregates

public:
    std::uint64_t corruptCount() const { return corrupt_; }
};

} // namespace caendaq
