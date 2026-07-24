# CaenDAQ

A small, portable (Linux + Windows) C++17 DAQ for CAEN digitizers, intended to
replace the XDAQ + Docker + LunaSpy + socket chain used by WebDAQ. No XDAQ, no
Docker, no I2O, no ROOT on the C++ side.

## Design

```
  CAEN board ─► IDigitizer      reader thread ─┬─► BlockQueue ─► writer thread
                (CaenDigitizer/  (tight read(), │   (must not      (RawFileWriter:
                 MockDigitizer)   never blocks)  │    drop)          .caendat + XDAQ header
                                                 │                   + size splitting)
                                                 └─► DecodeStage (best-effort tap, own thread)
                                                     AggregateDecoder ─► HistogramStore
                                                     (PHA/PSD/waveform)  (spectra + waveforms,
                                                                          thread-safe snapshots)
```

* **`IDigitizer`** — hardware abstraction. Lifecycle mirrors the proven CAEN
  sequence: `open → configure → start → read* → stop → close`. No method throws;
  failures are returned as `bool` and logged. Two implementations:
  * `MockDigitizer` — fabricates CAEN-style aggregate buffers, so the entire
    pipeline runs and is testable **without hardware or CAEN libraries**.
  * `CaenDigitizer` — the real board (built only with `-DCAENDAQ_WITH_CAEN=ON`);
    a thin adapter over the vendored `class_caen_dgtz` driver, adding
    auto-reconnect and exposing `BoardInfo` for the file header.
* **`BlockQueue`** — bounded, thread-safe queue decoupling the time-critical
  reader from disk I/O. Full-queue blocks are dropped and **counted**, never
  silently lost, so a slow disk can't overflow memory or stall the board.
* **`RawFileWriter`** — appends the raw board buffer to
  `<dir>/ru_<board>_run<N>_<cyc>.caendat`, writes the XDAQ header on the first
  file, and rolls to a new cycle file past a configurable size limit.
* **`DecodeStage` / `AggregateDecoder` / `HistogramStore`** — the optional
  parallel decode tap (see below). Best-effort, so it never slows readout.
* **`BoardRunner`** — wires one board's reader + writer (+ optional decode)
  together. Every thread body is wrapped so no exception can escape and crash the
  process.

### On-disk format (`.caendat`)

The **exact XDAQ file header** (`rubuilder/rucaendgtz/ReadoutUnit.{h,cc}`), so the
existing **RUReader reads CaenDAQ files with no changes** — including
self-identifying the board from the header's board-info words. A little-endian
`uint32` word array, written once to the first file of a run; split/continuation
files start directly with a board aggregate. Defined in
`include/caendaq/DataFileHeader.hpp`:

```
[0] hSize = 6 + nbBoards          [1] keyFrame = 0xFA0201A0   [2] nbBoards
[3] ts_0 = 0                      [4] ts_1 = 0                [5] epochStartTime
[6+i] boardDef[i]:  nsPerSample[5:0] | nsPerTimetag[11:6] | dppVersion[15:12]
                    | channels[23:16] | boardRegId[31:24]     (dpp: 0=PHA, 1=PSD)
<raw CAEN aggregate stream, block after block, across all cycle files>
```

No trailer — RUReader reads aggregates until EOF, so nothing may follow the
payload. `--no-header` emits pure payload (continuation-file style).

> Verified: building RUReader locally and running it on a CaenDAQ file prints the
> correct board table ("V1730 (from header), 16 channels, PSD, ns per sample 2")
> and converts to ROOT successfully.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Mock-only build needs just a C++17 compiler + threads (no external deps). For a
real board add `-DCAENDAQ_WITH_CAEN=ON` (requires `libCAENDigitizer` **and**
`jsoncpp`, which the vendored driver uses to parse its register-config files).

## Run (mock source — no hardware needed)

```bash
# 3-second run, split every 1 MB, into data/run42/
./build/caendaq_cli --run 42 --dir data/run42 --board V1730A \
                    --maxsize 1000000 --seconds 3 --rate 500
```

## Run (real CAEN board)

Build with `-DCAENDAQ_WITH_CAEN=ON`, then:

```bash
# --caen <conn> <link> <node> <base>   conn: 0=USB, 1=Optical, 5=A4818, ...
./build/caendaq_cli --run 42 --dir data/run42 --board V1730A \
    --config /path/V1730_PSD.json --maxsize 1000000000 \
    --caen 5 0 0 0
```

The register-config JSON is the **same format** the XDAQ DAQ uses (global keys +
per-channel `chN` objects) — the vendored driver parses it unchanged.

## Parallel decode (online spectra)

Add `--decode` to fill online spectra while acquiring. The reader submits each
raw buffer to BOTH the disk writer (must not drop) and a **best-effort decode
tap** on its own thread: if the decoder falls behind it drops (counted) rather
than back-pressuring the readout, so **data is never lost to disk**.

```bash
./build/caendaq_cli --run 9 --dir data/run9 --board V1730A --decode --seconds 5
```

The decoder (`AggregateDecoder`) walks the raw CAEN aggregates using RUReader's
vendored per-model bit tables (`vendor/rureader/`) and emits `DecodedEvent`s
(board, channel, energy | qshort/qlong, pileup, waveform). `HistogramStore`
accumulates per-channel energy/qshort/qlong spectra + the latest waveform as
plain integer arrays, with thread-safe snapshots — no ROOT on the C++ side.

Run the tests: `cd build && ctest` (a crafted V1730 PSD aggregate is decoded and
checked field-by-field).

## Statistics & Graphite

Whenever decode is on, a `StatsCollector` thread samples the per-channel counters
each interval and computes **rates**: event, pile-up, lost and saturation rate
per channel, plus the file **write rate** per board. WebDAQ reads them via
`daq.stats()` (a list of per-board dicts), and — independently, like LunaSpy —
they are pushed to a Graphite/Carbon server if one is configured:

```python
daq = caendaq.DAQ("data/run42", run=42,
                  graphite_host="172.18.9.54", graphite_port=2003)
daq.add_board("V1730A", mock=True, decode=True, write=False)  # write=False = decode-only
...
for b in daq.stats():
    for c in b["channels"]:
        print(c["channel"], c["event_rate"], c["pileup_rate"])
```

Carbon lines match LunaSpy's naming: `ancillary.rates.<board>.bo_<b>.ch_<c>.totalRate <v> <epoch>`
(plus `pileRate`/`lostRate`/`satuRate`, and `<board>.writeRate`). `write=False`
gives a monitoring-only run (spectra + rates, no `.caendat` files).

## Python module (WebDAQ integration)

Build the pybind11 module and use the DAQ as an in-process instance — no socket,
no ROOT on the C++ side, histograms come back as numpy arrays:

```bash
cmake -S . -B build -DCAENDAQ_BUILD_PYTHON=ON \
      -Dpybind11_DIR="$(python3 -m pybind11 --cmakedir)"
cmake --build build -j          # produces build/caendaq.*.so
```

```python
import caendaq
daq = caendaq.DAQ("data/run42", run=42, max_file_bytes=1_000_000_000)
daq.add_board("V1730A", conn_type=5, config="V1730_PSD.json", decode=True)  # 5=A4818
#   ... or a hardware-free source: add_board("V1730A", mock=True, decode=True)
daq.start()
...
qlong = daq.qlong(board=0, channel=3)     # numpy uint32, 65536 bins
wave  = daq.waveform(0, 3)                 # numpy int16 samples
daq.stop()
```

The lifecycle calls release the GIL, so acquisition + decode run on C++ threads
while Python stays responsive. See `python/example.py` for a full mock demo.

**Replacing `spy.py` in WebDAQ:** instead of a PyROOT `TSocket` client polling
`RUSpy` on port 6060, hold a `caendaq.DAQ` instance and read `daq.energy/qshort/
qlong/waveform(board, ch)` each poll. Feed those numpy arrays into a `TH1F` in
Python (ROOT stays only on the WebDAQ side) so `TBufferJSON` + the JSROOT frontend
are unchanged. XDAQ, Docker, LunaSpy and both sockets drop out.

## Roadmap

1. ✅ **Raw `.caendat` saving** — exact XDAQ header (board info), size-based
   splitting, robust two-thread pipeline. Validated against the real RUReader.
2. ✅ **`CaenDigitizer`** — real board `open → configure(JSON) → start → read →
   stop → close`, following the proven XDAQ sequence, with auto-reconnect on
   communication errors. Reuses the vendored `class_caen_dgtz` driver so all
   board-version quirks / calibration / DPP config are preserved.
3. ✅ **Parallel decoder** — non-blocking decode tap → energy/PSD histograms +
   waveforms as plain arrays. Reuses RUReader's vendored format tables.
4. ✅ **`pybind11` module** — `import caendaq; daq = caendaq.DAQ(...)`, exposing
   histograms/waveforms as numpy for WebDAQ (Python keeps ROOT only to feed
   JSROOT; no socket). Validated end-to-end: mock → .caendat → online decode →
   numpy spectra, cross-checked against RUReader (identical event counts).
5. ✅ **Statistics + Graphite + no-write mode** — per-channel event/pile-up/lost/
   saturation rates + file write rate, exposed via `daq.stats()` and pushed to
   Carbon independently; `write=False` for monitoring-only runs.
```
