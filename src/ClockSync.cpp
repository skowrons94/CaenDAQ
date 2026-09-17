#include "caendaq/ClockSync.hpp"

#include <chrono>
#include <thread>

#include "caendaq/Log.hpp"

namespace caendaq {

std::vector<std::size_t> clockChainOrder(const std::vector<std::uint32_t>& acquisitionControl,
                                         int masterIndex) {
    std::vector<std::size_t> order;
    bool anyExternal = false;
    for (auto acq : acquisitionControl)
        if (acq & kAcqControlExternalClock) anyExternal = true;
    if (!anyExternal) return order;

    const bool haveMaster = masterIndex >= 0 &&
                            static_cast<std::size_t>(masterIndex) < acquisitionControl.size();
    if (haveMaster) order.push_back(static_cast<std::size_t>(masterIndex));
    for (std::size_t i = 0; i < acquisitionControl.size(); ++i) {
        if (haveMaster && i == static_cast<std::size_t>(masterIndex)) continue;
        if (acquisitionControl[i] & kAcqControlExternalClock) order.push_back(i);
    }
    return order;
}

std::vector<ClockLockState> synchroniseClockChain(const std::vector<ClockChainBoard>& chain,
                                                  const ClockSyncOptions& options) {
    const auto sleep = options.sleep ? options.sleep : [](int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };

    std::vector<ClockLockState> states;
    states.reserve(chain.size());
    for (const auto& b : chain) {
        ClockLockState s;
        s.name   = b.name;
        s.master = b.master;
        states.push_back(s);
    }
    if (chain.empty()) return states;

    LOG_INFO("ClockSync: re-aligning the clock chain (" << chain.size()
             << " boards, master first) with Software Clock Sync 0x813C");

    // 1. Re-align, first to last. Each board's outputs move when it re-aligns,
    //    so the next board is only touched once its reference has settled.
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const auto& b = chain[i];
        states[i].synced = b.write && b.write(kRegSoftwareClockSync, 1u);
        if (!states[i].synced)
            LOG_WARN("ClockSync: " << b.name << ": write to 0x813C failed");
        sleep(options.settleMs);
    }

    // 2. Every board: wait for Board Ready, then require that the PLL stays
    //    locked for a short window. 0x8104[7] is sticky — reading 0xEF04
    //    re-arms it, so an unlock seen afterwards happened during the window.
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const auto& b = chain[i];
        auto& s = states[i];
        if (!b.read) continue;

        std::uint32_t status = 0;
        for (int waited = 0;; waited += options.pollMs) {
            if (b.read(kRegAcquisitionStatus, &status)) {
                s.statusRead = true;
                if (status & kAcqStatusBoardReady) break;
            }
            if (waited >= options.readyTimeoutMs) break;
            sleep(options.pollMs);
        }
        if (!s.statusRead) continue;
        s.ready = (status & kAcqStatusBoardReady) != 0;

        std::uint32_t ignored = 0;
        b.read(kRegVmeStatus, &ignored);
        sleep(options.stableMs);
        if (b.read(kRegAcquisitionStatus, &status)) {
            s.ready    = (status & kAcqStatusBoardReady) != 0;
            s.stable   = (status & kAcqStatusNoPllUnlock) != 0;
            s.external = (status & kAcqStatusExternalClock) != 0;
        } else {
            s.statusRead = false;
        }
    }

    // 3. Report.
    for (const auto& s : states) {
        if (s.ok()) {
            LOG_INFO("ClockSync: " << s.name << ": PLL locked"
                     << (s.master ? " (master, internal reference)" : " to CLK-IN"));
            continue;
        }
        std::string why;
        if (!s.synced)          why = "the re-align command was not accepted";
        else if (!s.statusRead) why = "its acquisition status (0x8104) could not be read";
        else if (!s.master && !s.external)
            why = "it is not taking its clock from CLK-IN (0x8104[5] = 0)";
        else if (!s.ready)      why = "it did not report Board Ready (0x8104[8]) within the timeout";
        else                    why = "its PLL lost lock again during the check (0x8104[7])";
        LOG_WARN("ClockSync: " << s.name << " is NOT locked to the clock chain: " << why
                 << ". Time tags of this board will not agree with the master"
                 << (s.master ? "" : "; check the CLK-OUT -> CLK-IN cable feeding it")
                 << ". Starting anyway.");
    }
    return states;
}

} // namespace caendaq
