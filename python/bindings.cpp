//
// pybind11 bindings — exposes the CaenDAQ orchestrator as an in-process Python
// instance. Histograms/waveforms come back as numpy arrays; no ROOT, no socket.
//
//   import caendaq
//   daq = caendaq.DAQ("data/run42", run=42, max_file_bytes=1_000_000_000)
//   daq.add_board("V1730A", conn_type=5, config="V1730_PSD.json", decode=True)
//   daq.start()
//   ...
//   e = daq.energy(0, 3)     # numpy uint32 spectrum for board 0, channel 3
//   daq.stop()
//
#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include <cstring>

#include "caendaq/Daq.hpp"

namespace py = pybind11;
using namespace caendaq;

namespace {

template <typename T>
py::array_t<T> toArray(const std::vector<T>& v) {
    py::array_t<T> a(static_cast<py::ssize_t>(v.size()));
    if (!v.empty()) std::memcpy(a.mutable_data(), v.data(), v.size() * sizeof(T));
    return a;
}

// Start/Stop Mode (0x8100[1:0]) in the same words the manuals and the dashboard use.
std::string startModeName(std::uint32_t mode) {
    switch (mode) {
        case kStartModeSW:        return "SW controlled";
        case kStartModeSInGpi:    return "S-IN/GPI controlled";
        case kStartModeFirstTrig: return "First trigger controlled";
        case kStartModeLVDS:      return "LVDS controlled";
        default:                  return "unknown";
    }
}

// Firmware name as it appears in WebDAQ's board configuration ("DPP-PHA"...).
std::string dppName(DppType t) {
    switch (t) {
        case DppType::Std: return "Standard";
        case DppType::PHA: return "DPP-PHA";
        case DppType::PSD: return "DPP-PSD";
        case DppType::CI:  return "DPP-CI";
        case DppType::ZLE: return "DPP-ZLE";
        case DppType::QDC: return "DPP-QDC";
        case DppType::DAW: return "DPP-DAW";
        default:           return "unknown";
    }
}

} // namespace

PYBIND11_MODULE(caendaq, m) {
    m.doc() = "CaenDAQ — portable CAEN digitizer DAQ (acquisition, .caendat writing, online decode)";

    // Provenance, recorded in the run metadata alongside the board info.
#ifdef CAENDAQ_VERSION
    m.attr("__version__") = CAENDAQ_VERSION;
#else
    m.attr("__version__") = "unknown";
#endif
    // Whether this build can talk to real hardware (vs mock-only).
#ifdef CAENDAQ_WITH_CAEN
    m.attr("HAS_CAEN") = true;
#else
    m.attr("HAS_CAEN") = false;
#endif

    // 2D PSD histogram geometry (x = qlong over [0, QLONG_MAX), y = PSD ratio [0,1]).
    m.attr("PSD_XBINS") = static_cast<int>(HistogramStore::kPsdXBins);
    m.attr("PSD_YBINS") = static_cast<int>(HistogramStore::kPsdYBins);
    m.attr("QLONG_MAX") = static_cast<int>(HistogramStore::kQLongMax);

    m.def("board_fail_meaning", []() {
        return std::string(
            "A board-FAIL aggregate means the digitizer set the FAIL bit (bit 26) "
            "in a board-aggregate header: it could not sustain the readout for that "
            "block — typically an internal/memory (buffer-full) error or a lost link. "
            "The affected board's data from that point may be incomplete; the run "
            "should be checked and usually restarted. Count resets at the start of "
            "each run.");
    }, "Human-readable meaning of the board-failure counter.");

    py::class_<Daq>(m, "DAQ")
        .def(py::init([](const std::string& output_dir, std::uint32_t run,
                         std::uint64_t max_file_bytes, bool write_header,
                         const std::string& graphite_host, int graphite_port,
                         const std::string& graphite_prefix,
                         int stats_interval_ms) {
                 Daq::Options o;
                 o.maxFileBytes    = max_file_bytes;
                 o.writeHeader     = write_header;
                 o.graphiteHost    = graphite_host;
                 o.graphitePort    = graphite_port;
                 if (!graphite_prefix.empty()) o.graphitePrefix = graphite_prefix;
                 o.statsIntervalMs = stats_interval_ms;
                 return std::make_unique<Daq>(output_dir, run, o);
             }),
             py::arg("output_dir"), py::arg("run") = 0,
             py::arg("max_file_bytes") = 0, py::arg("write_header") = true,
             py::arg("graphite_host") = "", py::arg("graphite_port") = 2003,
             py::arg("graphite_prefix") = "ancillary.rates",
             py::arg("stats_interval_ms") = 1000,
             "Create a DAQ writing .caendat files into output_dir for run `run`. "
             "Set graphite_host to also push rates to a Carbon server, under "
             "graphite_prefix — one subtree per experiment, e.g. "
             "'ancillary.rates.12c12c'. Boards appear below it as bo_<VME board "
             "id>, so the prefix never has to name a board.\n\n"
             "Synchronisation is NOT configured here: it comes from each board's "
             "own Acquisition Control register (0x8100). A board whose start mode "
             "is not 'SW controlled' is armed instead of started, and once every "
             "board is armed the master (board register id 0) fires a software "
             "trigger that propagates down the TRG-OUT -> TRG-IN chain.")

        .def("add_board",
             [](Daq& d, const std::string& name, int conn_type, int link, int node,
                std::uint32_t base, const std::string& config, bool mock, bool decode,
                bool write, std::uint32_t mock_rate, bool mock_waveforms,
                std::uint32_t mock_fail_every, const std::string& mock_dpp,
                std::uint32_t mock_start_mode) {
                 BoardSpec s;
                 s.params.name       = name;
                 s.params.connType   = conn_type;
                 s.params.linkNum    = link;
                 s.params.node       = node;
                 s.params.vmeBase    = base;
                 s.params.configPath = config;
                 s.mock     = mock;
                 s.decode   = decode;
                 s.write    = write;
                 s.mockRate = mock_rate;
                 s.mockWaveforms = mock_waveforms;
                 s.mockFailEvery = mock_fail_every;
                 // Mock firmware to emulate (PHA fills energy, PSD fills q).
                 // Accept the WebDAQ "DPP-PHA"/"DPP-PSD" spelling and bare names.
                 s.mockDpp = (mock_dpp == "DPP-PHA" || mock_dpp == "PHA" || mock_dpp == "pha")
                                 ? DppType::PHA : DppType::PSD;
                 s.mockStartMode = mock_start_mode;
                 return d.addBoard(s);
             },
             py::arg("name"), py::arg("conn_type") = 0, py::arg("link") = 0,
             py::arg("node") = 0, py::arg("base") = 0, py::arg("config") = "",
             py::arg("mock") = false, py::arg("decode") = true, py::arg("write") = true,
             py::arg("mock_rate") = 200, py::arg("mock_waveforms") = false,
             py::arg("mock_fail_every") = 0, py::arg("mock_dpp") = "DPP-PSD",
             py::arg("mock_start_mode") = 0,
             "Add a board (conn_type: 0=USB, 1=Optical, 5=A4818; write=False for "
             "decode-only; mock_dpp 'DPP-PHA'/'DPP-PSD' picks the mock firmware; "
             "mock_start_mode mirrors the board's 0x8100[1:0] so a mock run "
             "reproduces the configured chain). Returns its index.")

        // Lifecycle — release the GIL, these block on I/O / thread joins.
        .def("prepare", &Daq::prepare, py::call_guard<py::gil_scoped_release>())
        .def("start",   &Daq::start,   py::call_guard<py::gil_scoped_release>())
        .def("stop",    &Daq::stop,    py::call_guard<py::gil_scoped_release>())

        .def("set_graphite", &Daq::setGraphite, py::arg("host"), py::arg("port") = 2003,
             py::arg("prefix") = "",
             "Change the Graphite/Carbon target of the running stats collector "
             "(empty host disables it) and, optionally, the metric prefix — the "
             "experiment's subtree, e.g. 'ancillary.rates.12c12c'. An empty prefix "
             "leaves the current one alone.")

        .def("write_register", &Daq::writeRegister,
             py::arg("board"), py::arg("address"), py::arg("value"),
             py::call_guard<py::gil_scoped_release>(),
             "Write a register on an open board, including during a run — this is "
             "what online tuning uses to move a threshold and see the effect "
             "immediately. The access is serialised against the board's reader "
             "thread. Returns False if the board is out of range, not open, or the "
             "write failed.\n\n"
             "CaenDAQ does not decide which registers may be changed while data is "
             "being taken; the caller owns that policy.")
        .def("read_register",
             [](Daq& d, int board, std::uint32_t address) -> py::object {
                 std::uint32_t value = 0;
                 bool ok = false;
                 {
                     py::gil_scoped_release release;
                     ok = d.readRegister(board, address, &value);
                 }
                 if (!ok) return py::none();
                 return py::int_(value);
             },
             py::arg("board"), py::arg("address"),
             "Read a register back from an open board, or None if it could not be "
             "read. Use it after write_register to confirm what the board took.")

        .def_property_readonly("n_boards", &Daq::boardCount)
        .def_property_readonly("running",  &Daq::running)
        .def("board_name", &Daq::boardName, py::arg("board"))

        // Everything the CAEN API knows about a board, plus the acquisition
        // registers read back after configuration. Meant to be stored verbatim
        // in the run metadata so a run is reproducible from the record alone.
        .def("board_info", [](const Daq& d, int board) {
            const BoardInfo bi = d.boardInfo(board);
            py::dict o;
            o["model_name"]     = bi.modelName;
            o["model"]          = bi.model;
            o["family_code"]    = bi.familyCode;
            o["form_factor"]    = bi.formFactor;
            o["channels"]       = bi.channels;
            o["adc_bits"]       = bi.adcNBits;
            o["serial_number"]  = bi.serialNumber;
            o["pcb_revision"]   = bi.pcbRevision;
            o["board_reg_id"]   = bi.boardRegId;
            o["dpp_type"]       = dppName(bi.dppType);
            o["dpp_code"]       = static_cast<std::uint32_t>(bi.dppType);
            o["ns_per_sample"]  = bi.nsPerSample;
            o["ns_per_timetag"] = bi.nsPerTimetag;
            o["roc_firmware"]   = bi.rocFirmware;
            o["amc_firmware"]   = bi.amcFirmware;
            o["license"]        = bi.license;
            o["channel_enable_mask"]   = bi.channelEnableMask;
            o["acquisition_control"]   = bi.acquisitionControl;
            o["board_configuration"]   = bi.boardConfiguration;
            o["front_panel_io_control"] = bi.frontPanelIOControl;
            o["global_trigger_mask"]   = bi.globalTriggerMask;
            o["trg_out_enable_mask"]   = bi.trgOutEnableMask;
            o["run_delay"]             = bi.runDelay;
            // Start/Stop Mode (0x8100[1:0]) — what decides whether this board is
            // armed for a synchronised start or started by software.
            o["start_mode"]            = bi.acquisitionControl & 0x3u;
            o["start_mode_name"]       = startModeName(bi.acquisitionControl & 0x3u);
            o["sync_role"]             = std::string(toString(bi.syncRole));
            o["conn_type"]      = bi.connType;
            o["link_num"]       = bi.linkNum;
            o["node"]           = bi.node;
            o["vme_base"]       = bi.vmeBase;
            return o;
        }, py::arg("board"),
           "Full board identity, firmware and acquisition registers as read back "
           "from the hardware. Valid after prepare()/start().")

        // Spectra / waveform snapshots as numpy arrays.
        .def("energy",  [](Daq& d, int b, int ch) { return toArray(d.histograms(b).energy(b, ch)); },
             py::arg("board"), py::arg("channel"), "Energy spectrum (DPP-PHA), uint32 bins.")
        .def("qshort",  [](Daq& d, int b, int ch) { return toArray(d.histograms(b).qshort(b, ch)); },
             py::arg("board"), py::arg("channel"), "Short-gate charge spectrum (DPP-PSD).")
        .def("qlong",   [](Daq& d, int b, int ch) { return toArray(d.histograms(b).qlong(b, ch)); },
             py::arg("board"), py::arg("channel"), "Long-gate charge spectrum (DPP-PSD).")
        .def("psd", [](Daq& d, int b, int ch) {
                 auto flat = d.histograms(b).psd(b, ch);
                 const auto nx = static_cast<py::ssize_t>(HistogramStore::kPsdXBins);
                 const auto ny = static_cast<py::ssize_t>(HistogramStore::kPsdYBins);
                 if (flat.empty()) return py::array_t<std::uint32_t>(std::vector<py::ssize_t>{0, 0});
                 py::array_t<std::uint32_t> a(std::vector<py::ssize_t>{nx, ny});
                 std::memcpy(a.mutable_data(), flat.data(), flat.size() * sizeof(std::uint32_t));
                 return a;
             }, py::arg("board"), py::arg("channel"),
             "2D DPP-PSD histogram (uint32, shape [PSD_XBINS, PSD_YBINS]); empty until a PSD event.")
        .def("waveform",[](Daq& d, int b, int ch) { return toArray(d.histograms(b).waveform(b, ch)); },
             py::arg("board"), py::arg("channel"), "Latest waveform (int16 samples).")
        .def("channel_events",
             [](Daq& d, int b, int ch) { return d.histograms(b).events(b, ch); },
             py::arg("board"), py::arg("channel"), "Number of events seen on (board, channel).")
        .def("active_channels",
             [](Daq& d, int b) { return d.histograms(b).channels(); }, py::arg("board"),
             "List of (board, channel) pairs that have received events.")

        // Live counters.
        .def("buffers_read",   &Daq::buffersRead,   py::arg("board"))
        .def("bytes_read",     &Daq::bytesRead,     py::arg("board"))
        .def("bytes_written",  &Daq::bytesWritten,  py::arg("board"))
        .def("blocks_dropped", &Daq::blocksDropped, py::arg("board"))
        .def("comm_errors",    &Daq::commErrors,    py::arg("board"))
        .def("events_decoded", &Daq::eventsDecoded, py::arg("board"))
        .def("board_failures", &Daq::boardFailures, py::arg("board"),
             "Cumulative board-FAIL aggregates this run (0 = healthy). Resets each run.")

        // Latest per-board/per-channel rates (events/pileup/lost/satu per second
        // + file write rate). Returns a list of dicts, one per board.
        .def("stats", [](const Daq& d) {
            py::list boards;
            for (const auto& b : d.stats()) {
                py::dict bd;
                bd["name"] = b.name;
                bd["board_id"] = b.boardRegId;   // VME board id — the 'bo_' in metric paths
                bd["write_rate"] = b.writeRate;
                bd["board_failures"] = b.boardFail;   // cumulative this run
                bd["failed"] = (b.boardFail > 0);
                py::list chans;
                for (const auto& c : b.channels) {
                    py::dict cd;
                    cd["board"]  = c.board;
                    cd["channel"] = c.ch;
                    cd["event_rate"]  = c.events;
                    cd["pileup_rate"] = c.pileup;
                    cd["lost_rate"]   = c.lost;
                    cd["satu_rate"]   = c.satu;
                    chans.append(cd);
                }
                bd["channels"] = chans;
                boards.append(bd);
            }
            return boards;
        }, "Latest per-board/per-channel rates as a list of dicts.");
}
