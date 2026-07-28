//
// caendaq_cli — standalone runner for the raw-data (.caendat) saving pipeline.
//
// Reads one or more boards and appends their buffers to ONE unified .caendat
// file (run_<run>_<part>.caendat) with the XDAQ-compatible multi-board header
// and size-based splitting. Uses the synthetic MockDigitizer by default so the
// whole pipeline is verifiable without hardware; pass
// --caen <conn> <link> <node> <base> to drive a real board (requires a build
// with -DCAENDAQ_WITH_CAEN=ON).
//
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "caendaq/BoardInfo.hpp"
#include "caendaq/Daq.hpp"
#include "caendaq/Log.hpp"

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop.store(true); }

void usage(const char* argv0) {
    std::cout <<
        "Usage: " << argv0 << " [options]\n"
        "  --run <N>            run number (default 0)\n"
        "  --dir <path>         output directory (default ./data)\n"
        "  --board <name>       board name (default V1730)\n"
        "  --dpp <pha|psd>      mock firmware to emulate (default psd)\n"
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
    std::string   dir       = "./data";
    std::uint32_t run       = 0;
    std::uint64_t maxSize   = 0;
    bool          writeHead = true;
    bool          decode    = false;
    std::uint32_t runSeconds = 0;
    std::uint32_t mockRate   = 200;
    bool          useCaen    = false;

    caendaq::BoardSpec spec;
    spec.params.name = "V1730";
    spec.mock   = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](int n) { return i + n < argc; };
        if (a == "--run" && need(1))            run = static_cast<std::uint32_t>(parseU64(argv[++i]));
        else if (a == "--dir" && need(1))       dir = argv[++i];
        else if (a == "--board" && need(1))     spec.params.name = argv[++i];
        else if (a == "--dpp" && need(1)) {
            std::string v = argv[++i];
            spec.mockDpp = (v == "pha" || v == "PHA" || v == "DPP-PHA")
                               ? caendaq::DppType::PHA : caendaq::DppType::PSD;
        }
        else if (a == "--config" && need(1))    spec.params.configPath = argv[++i];
        else if (a == "--maxsize" && need(1))   maxSize = parseU64(argv[++i]);
        else if (a == "--seconds" && need(1))   runSeconds = static_cast<std::uint32_t>(parseU64(argv[++i]));
        else if (a == "--rate" && need(1))      mockRate = static_cast<std::uint32_t>(parseU64(argv[++i]));
        else if (a == "--no-header")            writeHead = false;
        else if (a == "--decode")               decode = true;
        else if (a == "--caen" && need(4)) {
            useCaen = true;
            spec.mock = false;
            spec.params.connType = static_cast<int>(parseU64(argv[++i]));
            spec.params.linkNum  = static_cast<int>(parseU64(argv[++i]));
            spec.params.node     = static_cast<int>(parseU64(argv[++i]));
            spec.params.vmeBase  = static_cast<std::uint32_t>(parseU64(argv[++i]));
        } else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::cerr << "Unknown/incomplete option: " << a << "\n"; usage(argv[0]); return 2; }
    }
    (void)useCaen;
    spec.decode   = decode;
    spec.mockRate = mockRate;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    caendaq::Daq::Options opt;
    opt.maxFileBytes = maxSize;
    opt.writeHeader  = writeHead;
    caendaq::Daq daq(dir, run, opt);
    if (daq.addBoard(spec) < 0) { std::cerr << "addBoard() failed\n"; return 1; }
    if (!daq.start())           { std::cerr << "start() failed\n";   return 1; }

    const auto t0 = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        LOG_INFO("[" << daq.boardName(0) << "] buffers=" << daq.buffersRead(0)
                 << " bytes=" << daq.bytesRead(0)
                 << " written=" << daq.bytesWritten(0)
                 << " dropped=" << daq.blocksDropped(0)
                 << " commErr=" << daq.commErrors(0)
                 << (decode ? (" events=" + std::to_string(daq.eventsDecoded(0))) : std::string()));
        if (runSeconds != 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - t0).count();
            if (elapsed >= static_cast<long>(runSeconds)) break;
        }
    }

    LOG_INFO("stopping...");
    daq.stop();
    std::cout << "Done. Read " << daq.bytesRead(0) << " bytes over "
              << daq.buffersRead(0) << " buffers.\n";

    if (decode) {
        auto& h = daq.histograms(0);
        const auto chans = h.channels();
        std::cout << "Decoded " << daq.eventsDecoded(0) << " events over "
                  << chans.size() << " channel(s).\n";
        for (const auto& bc : chans) {
            std::cout << "  board " << bc.first << " ch " << bc.second
                      << ": events=" << h.events(bc.first, bc.second)
                      << " waveLen=" << h.waveform(bc.first, bc.second).size() << "\n";
        }
    }
    return 0;
}
