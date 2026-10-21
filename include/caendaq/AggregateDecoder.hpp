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
    // `psPerTick` is the time-stamp counter period, which sets DecodedEvent::timePs
    // (0 leaves timePs at 0).
    void registerBoard(std::uint32_t boardRegId, DppType dpp, const std::string& modelName,
                       std::uint32_t psPerTick = 0);

    // Convenience: register straight from a BoardInfo.
    void registerBoard(const BoardInfo& bi) {
        registerBoard(bi.boardRegId, bi.dppType, bi.modelName, timeStampPs(bi));
    }

    // Period of a board's time-stamp counter, in ps. The counter runs at the
    // sampling clock: the "ns per time tag" CAEN reports is not usable for it
    // (a V1725 DPP-PHA declares 16 ns while ticking at 4 ns, see RUReader's
    // BoardTimeStampLsb). Falls back on the model name when nsPerSample is unset.
    static std::uint32_t timeStampPs(const BoardInfo& bi);

    // Decode a raw buffer (bytes). Calls `fn` for every event. Returns the number
    // of events emitted. Never throws.
    std::size_t decode(const char* data, std::size_t nbytes, const EventFn& fn);

private:
    void decodePHA(const std::uint32_t* buf, std::uint32_t board, std::bitset<8> mask,
                   std::uint32_t offset, std::uint32_t aggLen, const EventFn& fn);
    void decodePSD(const std::uint32_t* buf, std::uint32_t board, std::bitset<8> mask,
                   std::uint32_t offset, std::uint32_t aggLen, const EventFn& fn);
    // Fills wave_ with trace 1, whose first word is buf[first], and returns the
    // sample count (0 if none).
    std::uint32_t unpackWave(const std::uint32_t* buf, const DataLayout& layout,
                             std::uint32_t first);

    // Places a raw counter value in time. The boards do not sort their output,
    // so a wrap is never inferred from the previous event: the event goes in
    // whichever epoch puts it closest to where the board already is (as
    // RUReader::BuildTimeStamp does, without the roll-over markers). All the
    // channels of a board count on one clock, so the reference is the board's:
    // a channel silent for longer than half a wrap still lands in the right
    // epoch. An event read more than half a wrap after its board moved on would
    // not, but the readout never holds one back that long.
    struct ChannelClock {
        std::uint64_t epoch = 0;
        std::uint64_t reference = 0;
        bool          seen = false;
    };
    std::uint64_t unwrap(ChannelClock& c, std::uint64_t raw, int width);
    void setTime(DecodedEvent& e, std::uint32_t board, int width, std::uint32_t fineTS, int fineBits);

    struct BoardDec {
        DataFrame frame;
        bool      isPHA = false;
        bool      supported = false;
        std::uint32_t psPerTick = 0;
        ChannelClock  clock;
    };

    std::map<std::uint32_t, BoardDec> boards_;
    std::vector<std::int16_t>         wave_;   // reusable trace buffer
    std::uint64_t                     corrupt_   = 0; // skipped/corrupt aggregates
    std::uint64_t                     boardFail_ = 0; // aggregates with the board-fail bit set
    std::uint64_t                     uninitialised_       = 0; // channel aggregates of random words
    std::uint64_t                     uninitialisedEvents_ = 0; // the events they claimed
    std::uint64_t                     incomplete_ = 0; // events without a time stamp or energy word

public:
    std::uint64_t corruptCount() const { return corrupt_; }
    // Channel aggregates whose two-word header was valid but whose payload was
    // random — the board never wrote it. Their events are NOT decoded and do
    // not reach the rates. Cumulative for the life of this decoder (one run).
    std::uint64_t uninitialisedCount() const { return uninitialised_; }
    std::uint64_t uninitialisedEvents() const { return uninitialisedEvents_; }
    // Number of aggregates whose header carried the board-FAIL bit (bit 26):
    // the board signalled it could not sustain the readout (buffer/internal
    // error). Cumulative for the life of this decoder (i.e. one run).
    std::uint64_t boardFailCount() const { return boardFail_; }
    // Events the board sent without a time stamp or an energy/charge word: its
    // Board Configuration (0x8000) has time stamp or energy recording (bits
    // 18/19, "must be 1") switched off. Their energy and time are left at 0.
    std::uint64_t incompleteCount() const { return incomplete_; }
};

} // namespace caendaq
