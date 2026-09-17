// Daisy-chain clock alignment (UM6769 "Software Clock Sync", 0x813C).
//
// What matters on the experiment: the re-align reaches every board in the
// clock chain, master first and down the chain in order, only boards that
// really take an external clock are touched, and a slave whose PLL is not
// locked is reported instead of passing silently — while the run still starts.
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "caendaq/ClockSync.hpp"

using namespace caendaq;

// A board that answers register reads from a fixed status, records every write
// into one shared, ordered log, and can be told to fail its lock checks.
struct FakeBoard {
    std::string name;
    std::uint32_t status = kAcqStatusBoardReady | kAcqStatusNoPllUnlock | kAcqStatusExternalClock;
    bool writeFails = false;
    // After 0xEF04 re-arms the unlock latch, report an unlock during the window.
    bool unlocksDuringCheck = false;
    bool armed = false;
    std::vector<std::string>* log = nullptr;

    ClockChainBoard chain(bool master) {
        ClockChainBoard b;
        b.name = name;
        b.master = master;
        b.write = [this](std::uint32_t a, std::uint32_t) {
            log->push_back(name + ":w" + std::to_string(a));
            return !writeFails;
        };
        b.read = [this](std::uint32_t a, std::uint32_t* v) {
            if (a == kRegVmeStatus) { armed = true; *v = 0; return true; }
            std::uint32_t s = status;
            if (a == kRegAcquisitionStatus && armed && unlocksDuringCheck) s &= ~kAcqStatusNoPllUnlock;
            *v = s;
            return true;
        };
        return b;
    }
};

static ClockSyncOptions fast() {
    ClockSyncOptions o;
    o.sleep = [](int) {};
    return o;
}

int main() {
    // ---- 1. Which boards form the chain, and in what order -----------------
    {
        const std::uint32_t master = 0x02, slave = 0x42, independent = 0x00;
        // LUNA: board 0 master on its oscillator, boards 1 and 2 on CLK-IN.
        auto order = clockChainOrder({master, slave, slave}, 0);
        assert((order == std::vector<std::size_t>{0, 1, 2}));
        // The master is first even when it is not the first board added.
        order = clockChainOrder({slave, master, slave}, 1);
        assert((order == std::vector<std::size_t>{1, 0, 2}));
        // A board on its own oscillator that is not the master is left alone.
        order = clockChainOrder({master, independent, slave}, 0);
        assert((order == std::vector<std::size_t>{0, 2}));
        // No board takes an external clock: there is no chain to align.
        assert(clockChainOrder({master, independent, 0x01}, 0).empty());
        std::puts("chain order: ok");
    }

    // ---- 2. Re-align master first, then down the chain ----------------------
    {
        std::vector<std::string> log;
        FakeBoard b0{"b0"}, b1{"b1"}, b2{"b2"};
        b0.status &= ~kAcqStatusExternalClock;          // master: internal reference
        for (auto* b : {&b0, &b1, &b2}) b->log = &log;
        auto states = synchroniseClockChain({b0.chain(true), b1.chain(false), b2.chain(false)}, fast());

        const std::string w = ":w" + std::to_string(kRegSoftwareClockSync);
        assert((log == std::vector<std::string>{"b0" + w, "b1" + w, "b2" + w}));
        assert(states.size() == 3);
        for (auto& s : states) assert(s.ok());
        std::puts("healthy chain: re-aligned in order, all locked: ok");
    }

    // ---- 3. A slave that unlocks during the check is reported ---------------
    {
        std::vector<std::string> log;
        FakeBoard b0{"b0"}, b1{"b1"}, b2{"b2"};
        b0.status &= ~kAcqStatusExternalClock;
        b1.unlocksDuringCheck = true;
        for (auto* b : {&b0, &b1, &b2}) b->log = &log;
        auto s = synchroniseClockChain({b0.chain(true), b1.chain(false), b2.chain(false)}, fast());
        assert(s[0].ok() && s[2].ok());
        assert(!s[1].ok() && s[1].ready && !s[1].stable);
        std::puts("slave losing lock is flagged: ok");
    }

    // ---- 4. A slave not on CLK-IN, or never ready, is reported --------------
    {
        std::vector<std::string> log;
        FakeBoard b0{"b0"}, b1{"b1"}, b2{"b2"};
        b0.status &= ~kAcqStatusExternalClock;
        b1.status &= ~kAcqStatusExternalClock;          // slave running on its own oscillator
        b2.status &= ~kAcqStatusBoardReady;             // PLL never ready
        for (auto* b : {&b0, &b1, &b2}) b->log = &log;
        auto s = synchroniseClockChain({b0.chain(true), b1.chain(false), b2.chain(false)}, fast());
        assert(s[0].ok());
        assert(!s[1].ok() && !s[1].external);
        assert(!s[2].ok() && !s[2].ready);
        std::puts("slave off CLK-IN / never ready are flagged: ok");
    }

    // ---- 5. A failed write is reported and does not stop the procedure ------
    {
        std::vector<std::string> log;
        FakeBoard b0{"b0"}, b1{"b1"};
        b0.status &= ~kAcqStatusExternalClock;
        b0.writeFails = true;
        for (auto* b : {&b0, &b1}) b->log = &log;
        auto s = synchroniseClockChain({b0.chain(true), b1.chain(false)}, fast());
        assert(log.size() == 2);                         // b1 was still re-aligned
        assert(!s[0].ok() && !s[0].synced);
        assert(s[1].ok());
        std::puts("failed write reported, chain continues: ok");
    }

    std::puts("clock_sync_test: all passed");
    return 0;
}
