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

} // namespace

PYBIND11_MODULE(caendaq, m) {
    m.doc() = "CaenDAQ — portable CAEN digitizer DAQ (acquisition, .caendat writing, online decode)";

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
                         int stats_interval_ms) {
                 Daq::Options o;
                 o.maxFileBytes    = max_file_bytes;
                 o.writeHeader     = write_header;
                 o.graphiteHost    = graphite_host;
                 o.graphitePort    = graphite_port;
                 o.statsIntervalMs = stats_interval_ms;
                 return std::make_unique<Daq>(output_dir, run, o);
             }),
             py::arg("output_dir"), py::arg("run") = 0,
             py::arg("max_file_bytes") = 0, py::arg("write_header") = true,
             py::arg("graphite_host") = "", py::arg("graphite_port") = 2003,
             py::arg("stats_interval_ms") = 1000,
             "Create a DAQ writing .caendat files into output_dir for run `run`. "
             "Set graphite_host to also push rates to a Carbon server.")

        .def("add_board",
             [](Daq& d, const std::string& name, int conn_type, int link, int node,
                std::uint32_t base, const std::string& config, bool mock, bool decode,
                bool write, std::uint32_t mock_rate, bool mock_waveforms,
                std::uint32_t mock_fail_every, const std::string& mock_dpp) {
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
                 return d.addBoard(s);
             },
             py::arg("name"), py::arg("conn_type") = 0, py::arg("link") = 0,
             py::arg("node") = 0, py::arg("base") = 0, py::arg("config") = "",
             py::arg("mock") = false, py::arg("decode") = true, py::arg("write") = true,
             py::arg("mock_rate") = 200, py::arg("mock_waveforms") = false,
             py::arg("mock_fail_every") = 0, py::arg("mock_dpp") = "DPP-PSD",
             "Add a board (conn_type: 0=USB, 1=Optical, 5=A4818; write=False for "
             "decode-only; mock_dpp 'DPP-PHA'/'DPP-PSD' picks the mock firmware). "
             "Returns its index.")

        // Lifecycle — release the GIL, these block on I/O / thread joins.
        .def("prepare", &Daq::prepare, py::call_guard<py::gil_scoped_release>())
        .def("start",   &Daq::start,   py::call_guard<py::gil_scoped_release>())
        .def("stop",    &Daq::stop,    py::call_guard<py::gil_scoped_release>())

        .def("set_graphite", &Daq::setGraphite, py::arg("host"), py::arg("port") = 2003,
             "Change the Graphite/Carbon target of the running stats collector "
             "(empty host disables it).")

        .def_property_readonly("n_boards", &Daq::boardCount)
        .def_property_readonly("running",  &Daq::running)
        .def("board_name", &Daq::boardName, py::arg("board"))

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
