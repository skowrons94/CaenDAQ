# CaenDAQ

A small, portable, in-process C++17 DAQ for CAEN digitizers, used by WebDAQ. It
reads the boards, writes `.caendat` files and decodes events to online spectra —
no I2O, no sockets, no ROOT on the C++ side.

## Design

```
  board 0 ─► IDigitizer  reader ─┐
  board 1 ─► IDigitizer  reader ─┼─► ONE shared BlockQueue ─► ONE writer thread
             (CaenDigitizer/     │   (must not drop)           (RawFileWriter: one
              MockDigitizer)     │                              unified .caendat, XDAQ
                                 │                              multi-board header, splitting)
                                 └─► DecodeStage (per board, best-effort tap, own thread)
                                     AggregateDecoder ─► HistogramStore
                                     (PHA/PSD/waveform)  (spectra + waveforms,
                                                          thread-safe snapshots)
```

All boards stream into ONE unified `.caendat` (each CAEN aggregate self-identifies
its board), exactly like the XDAQ ReadoutUnit — not one file per board. Decoding
stays per board (its own HistogramStore).

* **`IDigitizer`** — hardware abstraction. Lifecycle mirrors the proven CAEN
  sequence: `open → configure → start → read* → stop → close`. No method throws;
  failures are returned as `bool` and logged. Two implementations:
  * `MockDigitizer` — fabricates CAEN-style aggregate buffers, so the entire
    pipeline runs and is testable **without hardware or CAEN libraries**.
  * `CaenDigitizer` — the real board (compiled in when `libCAENDigitizer` is found);
    a thin adapter over the vendored `class_caen_dgtz` driver, adding
    auto-reconnect and exposing `BoardInfo` for the file header.
* **`BlockQueue`** — bounded, thread-safe queue decoupling the time-critical
  reader from disk I/O. Full-queue blocks are dropped and **counted**, never
  silently lost, so a slow disk can't overflow memory or stall the board.
* **`RawFileWriter`** — appends every board's raw buffers to one unified
  `<dir>/run_<N>_<part>.caendat`, writes the XDAQ multi-board header on the first
  file, and rolls to a new part file past a configurable size limit. Never
  overwrites an existing file (appends `_1`/`_2`/… if the name is taken).
* **`DecodeStage` / `AggregateDecoder` / `HistogramStore`** — the optional
  parallel decode tap (see below). Best-effort, so it never slows readout.
* **`BoardRunner`** — wires one board's reader (+ optional decode) and pushes its
  buffers to the shared writer queue owned by `Daq`. Every thread body is wrapped
  so no exception can escape and crash the process.

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

Needs just a C++17 compiler + threads (no external deps) for the mock backend.
The real-board backend is **detected automatically**: install `libCAENDigitizer`
**and** `jsoncpp` (the vendored driver uses it to parse register-config files)
and it is compiled in. Add `-DCAENDAQ_WITH_CAEN=ON` to turn a missing CAEN
install into a configure error instead of a silent mock-only build.

## Run (mock source — no hardware needed)

```bash
# 3-second run, split every 1 MB, into data/run42/
./build/caendaq_cli --run 42 --dir data/run42 --board V1730A \
                    --maxsize 1000000 --seconds 3 --rate 500
```

## Run (real CAEN board)

Build on a machine with `libCAENDigitizer` installed (use
`-DCAENDAQ_WITH_CAEN=ON` to confirm it was picked up), then:

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

### Board health / failure

The decoder watches the **board-FAIL bit** (bit 26 of every board-aggregate
header) — the digitizer signalling it could not sustain the readout (buffer-full
/ internal error / lost link). It's exposed to Python and **resets every run** (a
fresh `DAQ` is built per run):

```python
daq.board_failures(0)        # cumulative FAIL aggregates on board 0 this run (0 = healthy)
daq.stats()[0]["failed"]     # True once any FAIL was seen; also ["board_failures"]
caendaq.board_fail_meaning() # human-readable explanation of what a failure means
```

So the server can watch `board_failures`/`failed` during a run to drive alerts /
auto-restart, without touching the boards itself.

## Multi-board synchronisation

There is **no API option for this**. Whether a board takes part in a
synchronised start is decided by the board's *own* configuration — Acquisition
Control (`0x8100`) bits[1:0], as written by whoever produced the register dump
(in LUNA's case, the WebDAQ dashboard). CaenDAQ only reads it back and does the
right thing:

| `0x8100[1:0]` | What CaenDAQ does |
|---|---|
| `00` SW controlled | `SWStartAcquisition()` — the board runs immediately |
| `10` first trigger | **arm** it (`0x8100[2]=1`) and wait for TRG-IN |
| `01` S-IN/GPI, `11` LVDS | **arm** it and wait for the external signal |

Then, once **every** board is armed, the master fires a software trigger:

```
  board 0  MASTER              board 1               board 2
  first-trigger mode           first-trigger mode    first-trigger mode
  SendSWTrigger() ──TRG-OUT──► TRG-IN ──TRG-OUT──►   TRG-IN
```

That pulse is not acquired as an event; it just releases the run, so every board
starts on the same edge and shares one time origin.

### Ordering

The software trigger is sent **after the arming loop**, never during it. Arming
board 0 and triggering it immediately would start the chain while boards 1..N
were still being armed — they would miss the edge and begin late, with a
different time origin.

### The master

The board that fires the trigger is the one whose **board register id is 0**
(the CAEN convention). If no board reports id 0, the first synchronised board is
used instead. See `Daq::masterIndex()`.

### Notes

- Cable **TRG-OUT of each board into TRG-IN of the next**, in the order the
  boards were added.
- Leave a board in *SW controlled* mode to keep it out of the chain; it starts on
  its own and its timestamps are not comparable with the rest.
- *Run/Start/Stop Delay* (`0x8170`) compensates propagation down a long chain.
  It is a plain register — set it in the configuration like any other.
- After an auto-reconnect a synchronised board is **re-armed**, not restarted:
  it can only resume on the next start signal, and CaenDAQ logs a warning saying
  so.

Each board's resulting role is reported in `board_info(i)["sync_role"]`
(`master` / `slave` / `independent`) and its mode in `["start_mode_name"]`.

## Board information & provenance

`board_info(i)` returns everything the CAEN API knows about a board plus the
acquisition registers **read back from the hardware** after configuration —
intended to be stored verbatim in run metadata so a run is reproducible from
the record alone:

```python
caendaq.__version__          # which build of this module took the data
caendaq.HAS_CAEN             # False for a mock-only build

daq.board_info(0)
# {'model_name': 'V1730', 'model': 730, 'family_code': 11, 'form_factor': 2,
#  'channels': 16, 'adc_bits': 14, 'serial_number': 1234, 'pcb_revision': 3,
#  'board_reg_id': 0, 'dpp_type': 'DPP-PSD', 'dpp_code': 2,
#  'ns_per_sample': 2, 'ns_per_timetag': 8,
#  'roc_firmware': '4.25_...', 'amc_firmware': '136.44_...', 'license': '...',
#  'channel_enable_mask': 0xFFFF, 'acquisition_control': 0x1, ...
#  'sync_role': 'master', 'conn_type': 0, 'link_num': 0, 'node': 0, 'vme_base': 0}
```

## Install the Python module (`import caendaq`)

Install it **into the exact Python environment that runs your app** (e.g. the
`luna` conda env). Two ways:

### A. pip (recommended)

Builds via CMake (scikit-build-core) and installs into that env's site-packages:

```bash
conda activate luna              # the env your server runs in
pip install .                    # detects CAEN automatically
```

**The CAEN backend is auto-detected.** `CAENDAQ_WITH_CAEN` defaults to `AUTO`:
if `libCAENDigitizer` and `jsoncpp` are on the machine, the hardware backend is
compiled in; if not, you get a mock-only module that still works with
`TEST_FLAG=True`. The same command is correct on the DAQ box and on a laptop.

Force the issue when you want a build to fail loudly rather than silently
degrade — worth doing on the DAQ machine itself:

| Value | Behaviour |
|---|---|
| `AUTO` (default) | Use the real backend if found, else mock only |
| `ON` | Require it; **fail the configure** if it is missing |
| `OFF` | Never build it, even if installed |

```bash
pip install . --config-settings=cmake.define.CAENDAQ_WITH_CAEN=ON
```

Verify which backend you actually got:

```bash
python -c "import caendaq; print(caendaq.__file__, 'HAS_CAEN =', caendaq.HAS_CAEN)"
```

#### "libCAENDigitizer was not found" — but the headers *are* in `/usr/include`

This is the conda case. When a conda env has the compiler toolchain installed,
its activation scripts export `CONDA_BUILD_SYSROOT` and a `CMAKE_ARGS` carrying
`-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY`. CMake then resolves every
`find_path` *inside the env prefix*, so a perfectly good
`/usr/include/CAENDigitizer.h` is invisible. The build now retries the search
with that rerooting bypassed, and the error message prints `CMAKE_SYSROOT` /
`CMAKE_FIND_ROOT_PATH` so you can see it happening.

Because the default is `AUTO`, this failure is **silent** — you get a mock-only
module instead of an error. `caendaq.HAS_CAEN` is how you catch it, and the
configure log says `CAEN backend NOT available` with the list of what was
missing.

If the retry still misses, point the build at the installation explicitly:

```bash
pip install . \
  --config-settings=cmake.define.CAENDAQ_WITH_CAEN=ON \
  --config-settings=cmake.define.CAEN_DGTZ_ROOT=/usr
```

`CAEN_DGTZ_ROOT` (or the `$CAEN_DGTZ_ROOT` env var) is searched first, expecting
`include/` and `lib/` below it. `CAEN_DGTZ_INCLUDE_DIR` / `CAEN_DGTZ_LIBRARY`
still work if you need to name the two paths individually.

> A failed lookup is **cached**. After installing the CAEN libraries, delete
> `build/` (or the pip build dir) — otherwise CMake reuses the old `-NOTFOUND`
> and reports the same error forever.

### B. CMake + manual install

```bash
cmake -S . -B build -DCAENDAQ_BUILD_PYTHON=ON \
      -Dpybind11_DIR="$(python -m pybind11 --cmakedir)"
      # CAEN is auto-detected; add -DCAENDAQ_WITH_CAEN=ON to require it,
      # or -DCAENDAQ_WITH_CAEN=OFF to force a mock-only build
cmake --build build -j                 # produces build/caendaq.*.so
install -m 755 build/caendaq*.so "$(python -c 'import site; print(site.getsitepackages()[0])')/"
```

> The module MUST land in the site-packages of the **same** interpreter that
> launches the server. A frequent trap is building in one env and starting the
> server from another — then `import caendaq` fails. `python -c "import caendaq"`
> from your server's activated env is the definitive check.

## Using the module (WebDAQ integration)

Use the DAQ as an in-process instance — no socket, no ROOT on the C++ side,
histograms come back as numpy arrays:

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
