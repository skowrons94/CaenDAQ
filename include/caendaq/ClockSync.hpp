#pragma once
//
// ClockSync — the CAEN clock-alignment procedure for boards that share a
// daisy-chained sampling clock (CLK-OUT of one board into CLK-IN of the next).
//
// UM6769 (724/781/782 DPP-PHA), "Software Clock Sync", register 0x813C:
//
//   "in case of Daisy chain clock distribution among VME boards, during the
//    initialization and configuration, the reference clocks along the Daisy
//    chain can be unstable and a temporary loss of lock may occur in the PLLs;
//    although the lock is automatically recovered once the reference clocks
//    return stable, it is not guaranteed that the phase shift returns to a known
//    state. This command allows the board to restore the correct phase shift
//    between the CLK-IN and the internal clocks.
//    NOTE: the command must be issued starting from the first to the last board
//    in the clock chain."
//
// Configuration is exactly that unstable period: every board has just had its
// register dump applied, including the clock source of the slaves. So once all
// boards are configured and before any is armed, each board in the chain is
// told to re-align, master first, and then every board is checked for a PLL
// that is ready and has stayed locked.
//
// A board that fails the check is reported, not refused. Starting anyway keeps
// the experiment running; the warning names the board and the likely cause, and
// the board-FAIL flag in the data still marks the affected aggregates.
//
// The procedure talks to the boards only through read/write callbacks, so it is
// the same code for real hardware and for tests. Never throws.
//
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace caendaq {

constexpr std::uint32_t kRegSoftwareClockSync = 0x813C; // W: any value re-aligns the PLL outputs
constexpr std::uint32_t kRegAcquisitionStatus = 0x8104; // R
constexpr std::uint32_t kRegVmeStatus         = 0xEF04; // R: reading it re-arms 0x8104[7]
constexpr std::uint32_t kRegAcqControl        = 0x8100;

constexpr std::uint32_t kAcqStatusExternalClock = 1u << 5; // PLL reference is CLK-IN
constexpr std::uint32_t kAcqStatusNoPllUnlock   = 1u << 7; // no unlock since 0xEF04 was read
constexpr std::uint32_t kAcqStatusBoardReady    = 1u << 8; // PLL and ADCs synchronised
constexpr std::uint32_t kAcqControlExternalClock = 1u << 6; // 0x8100[6]: take the clock from CLK-IN

struct ClockChainBoard {
    std::string name;
    bool        master = false; // first in the chain, on its own oscillator
    std::function<bool(std::uint32_t address, std::uint32_t value)>  write;
    std::function<bool(std::uint32_t address, std::uint32_t* value)> read;
};

struct ClockSyncOptions {
    int settleMs      = 300;  // after each board's re-align, before the next board
    int readyTimeoutMs = 2000; // per board, waiting for Board Ready
    int pollMs        = 50;
    int stableMs      = 200;  // the PLL must stay locked for this long to pass
    // Injected by tests to run without real waiting. Defaults to sleep_for.
    std::function<void(int ms)> sleep;
};

struct ClockLockState {
    std::string name;
    bool master     = false;
    bool synced     = false; // the 0x813C write was accepted
    bool statusRead = false; // 0x8104 could be read at all
    bool ready      = false; // 0x8104[8]
    bool stable     = false; // 0x8104[7] stayed set over stableMs
    bool external   = false; // 0x8104[5]

    // Master runs on its own oscillator; every other board must take CLK-IN.
    bool ok() const {
        return synced && statusRead && ready && stable && (master || external);
    }
};

// Which boards form the clock chain, as indices into the board list, in the
// order the re-align must be issued: the master first, then every board whose
// Acquisition Control selects the external clock (0x8100[6]), in board order.
// Empty when no board takes an external clock — there is no chain to align.
std::vector<std::size_t> clockChainOrder(const std::vector<std::uint32_t>& acquisitionControl,
                                         int masterIndex);

// Run the procedure over `chain` (already in chain order). Logs a summary line
// per board and a warning for every board that is not locked. Never throws.
std::vector<ClockLockState> synchroniseClockChain(const std::vector<ClockChainBoard>& chain,
                                                  const ClockSyncOptions& options = {});

} // namespace caendaq
