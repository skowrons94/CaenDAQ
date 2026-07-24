// Minimal correctness test: build one valid V1730 DPP-PSD board aggregate with
// known values and confirm AggregateDecoder extracts them.
//
// V1730 PSD layout (from vendored DataFrameBuilderPSD):
//   flags: Charge=bit30, TS=bit29, Extras=bit28, Trace=bit27
//   evtSize = (Charge?1) + (TS?1) + (Extras?1) + (Trace? nSamp/2)
//   event: word0 = TS[0:30] | CH<<31 ; last word = qshort[0:14] | pileup<<15 | qlong[16:31]
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "caendaq/AggregateDecoder.hpp"

int main() {
    using namespace caendaq;

    const std::uint16_t TS      = 0x1234;
    const std::uint16_t QSHORT  = 0x0ABC; // 2748
    const std::uint16_t QLONG   = 0x1DEF; // 7663

    const std::uint32_t dataFormat = (1u << 30) | (1u << 29); // Charge + TS, no extras/trace
    // evtSize = 2 words: [timeWord][chargeWord]
    const std::uint32_t timeWord   = TS;                                  // CH bit31 = 0 -> channel 0
    const std::uint32_t chargeWord = QSHORT | (static_cast<std::uint32_t>(QLONG) << 16);

    const std::uint32_t coupleSize = 2 + 2; // 2 header words + 1 event * evtSize(2)
    std::vector<std::uint32_t> buf;
    // board aggregate header (4 words)
    const std::uint32_t aggLen = 4 + coupleSize;
    buf.push_back(0xA0000000u | aggLen); // sync + length
    buf.push_back(0x00000001u);          // board 0, channelMask bit0 (couple 0)
    buf.push_back(0);                    // counter
    buf.push_back(0);                    // time tag
    // couple aggregate
    buf.push_back(coupleSize);
    buf.push_back(dataFormat);
    buf.push_back(timeWord);
    buf.push_back(chargeWord);

    AggregateDecoder dec;
    dec.registerBoard(/*boardRegId=*/0, DppType::PSD, "V1730");

    int seen = 0;
    DecodedEvent got{};
    const std::size_t n = dec.decode(
        reinterpret_cast<const char*>(buf.data()),
        buf.size() * sizeof(std::uint32_t),
        [&](const DecodedEvent& e) { got = e; ++seen; });

    printf("events=%zu seen=%d  board=%u ch=%u qshort=%u qlong=%u ts=%llu corrupt=%llu\n",
           n, seen, got.board, got.channel, got.qshort, got.qlong,
           (unsigned long long)got.timestamp, (unsigned long long)dec.corruptCount());

    assert(n == 1 && seen == 1);
    assert(got.board == 0);
    assert(got.channel == 0);
    assert(got.qshort == QSHORT);
    assert(got.qlong == QLONG);
    assert(got.timestamp == TS);
    assert(dec.corruptCount() == 0);
    printf("PASS: decoder extracted the known PSD event correctly.\n");
    return 0;
}
