//
// caendaq_cli — standalone runner for the raw-data (.caendat) saving pipeline.
//
// Deliverable 1: read board buffers -> append to .caendat with XDAQ-compatible
// header/trailer and size-based file splitting. Uses the synthetic MockDigitizer
// by default so the whole pipeline is verifiable without hardware; pass
// --caen <conn> <link> <node> <base> to drive a real board (requires a build
// with -DCAENDAQ_WITH_CAEN=ON).
//
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "caendaq/BoardRunner.hpp"
#include "caendaq/Log.hpp"
#include "caendaq/MockDigitizer.hpp"

#ifdef CAENDAQ_WITH_CAEN
#include "caendaq/CaenDigitizer.hpp"
#endif

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop.store(true); }

void usage(const char* argv0) {
    std::cout <<
        "Usage: " << argv0 << " [options]\n"
        "  --run <N>            run number (default 0)\n"
        "  --dir <path>         output directory (default ./data)\n"
        "  --board <name>       board name in filenames (default board0)\n"
        "  --config <file.json> register config file (real board only)\n"
        "  --maxsize <bytes>    split into new file past this size (0 = no split)\n"
        "  --seconds <S>        run for S seconds then stop (0 = until Ctrl-C)\n"
        "  --no-header          do not write the XDAQ file header (pure payload)\n"
        "  --decode             enable the parallel decode tap (online spectra)\n"
        "  --rate <N>           mock buffers per second (default 200)\n"
        "  --caen <conn> <link> <node> <base>\n"
        "                       use a real CAEN board instead of the mock source\n";
}

std::uint64_t parseU64(const char* s) { return std::strtoull(s, nullptr, 0); }

} // namespace

int main(int argc, char** argv) {
    caendaq::BoardParams params;
    params.name = "board0";

    caendaq::BoardRunnerConfig cfg;
    cfg.writer.directory = "./data";
    cfg.writer.prefix    = "ru";
    cfg.writer.maxFileBytes = 0;
    cfg.writer.writeHeader  = true;

    std::uint32_t runSeconds = 0;
    std::uint32_t mockRate   = 200;
    bool useCaen = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](int n) { return i + n < argc; };
        if (a == "--run" && need(1))            cfg.writer.runNumber = static_cast<std::uint32_t>(parseU64(argv[++i]));
        else if (a == "--dir" && need(1))       cfg.writer.directory = argv[++i];
        else if (a == "--board" && need(1))     params.name = argv[++i];
        else if (a == "--config" && need(1))    params.configPath = argv[++i];
        else if (a == "--maxsize" && need(1))   cfg.writer.maxFileBytes = parseU64(argv[++i]);
        else if (a == "--seconds" && need(1))   runSeconds = static_cast<std::uint32_t>(parseU64(argv[++i]));
        else if (a == "--rate" && need(1))      mockRate = static_cast<std::uint32_t>(parseU64(argv[++i]));
        else if (a == "--no-header")            cfg.writer.writeHeader = false;
        else if (a == "--decode")               cfg.decode = true;
        else if (a == "--caen" && need(4)) {
            useCaen = true;
            params.connType = static_cast<int>(parseU64(argv[++i]));
            params.linkNum  = static_cast<int>(parseU64(argv[++i]));
            params.node     = static_cast<int>(parseU64(argv[++i]));
            params.vmeBase  = static_cast<std::uint32_t>(parseU64(argv[++i]));
        } else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::cerr << "Unknown/incomplete option: " << a << "\n"; usage(argv[0]); return 2; }
    }
    cfg.writer.board = params.name;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::unique_ptr<caendaq::IDigitizer> dgtz;
    if (useCaen) {
#ifdef CAENDAQ_WITH_CAEN
        dgtz = std::make_unique<caendaq::CaenDigitizer>(params);
#else
        std::cerr << "This binary was built without CAEN support "
                     "(rebuild with -DCAENDAQ_WITH_CAEN=ON).\n";
        return 2;
#endif
    } else {
        caendaq::MockDigitizer::Options opt;
        opt.ratePerSec = mockRate;
        dgtz = std::make_unique<caendaq::MockDigitizer>(params, opt);
    }

    caendaq::BoardRunner runner(std::move(dgtz), cfg);
    if (!runner.prepare()) { std::cerr << "prepare() failed\n"; return 1; }
    if (!runner.start())   { std::cerr << "start() failed\n";   return 1; }

    const auto t0 = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        LOG_INFO("[" << runner.name() << "] buffers=" << runner.buffersRead()
                 << " bytes=" << runner.bytesRead()
                 << " dropped=" << runner.blocksDropped()
                 << " commErr=" << runner.commErrors()
                 << (runner.decoding()
                        ? (" events=" + std::to_string(runner.eventsDecoded())
                           + " decDrop=" + std::to_string(runner.decodeDropped()))
                        : std::string()));
        if (runSeconds != 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - t0).count();
            if (elapsed >= static_cast<long>(runSeconds)) break;
        }
    }

    LOG_INFO("stopping...");
    runner.stop();
    std::cout << "Done. Wrote " << runner.bytesRead() << " bytes over "
              << runner.buffersRead() << " buffers.\n";

    if (runner.decoding()) {
        const auto& h = runner.histograms();
        const auto chans = h.channels();
        std::cout << "Decoded " << runner.eventsDecoded() << " events over "
                  << chans.size() << " channel(s); decode drops="
                  << runner.decodeDropped() << ".\n";
        for (const auto& bc : chans) {
            std::cout << "  board " << bc.first << " ch " << bc.second
                      << ": events=" << h.events(bc.first, bc.second)
                      << " waveLen=" << h.waveform(bc.first, bc.second).size() << "\n";
        }
    }
    return 0;
}
