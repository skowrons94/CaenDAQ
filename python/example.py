#!/usr/bin/env python3
"""End-to-end demo of the CaenDAQ Python module (mock backend, no hardware).

Run from the build directory (which contains caendaq*.so):
    PYTHONPATH=build python3 python/example.py
"""
import time
import caendaq

daq = caendaq.DAQ("data/run100", run=100, max_file_bytes=5_000_000)
# A mock V1730 board with the parallel decoder enabled. For a real board:
#   daq.add_board("V1730A", conn_type=5, config="V1730_PSD.json", decode=True)
b = daq.add_board("V1730A", mock=True, decode=True, mock_rate=2000)
print(f"added board index {b}; starting...")

daq.start()
for _ in range(3):
    time.sleep(1.0)
    print(f"  buffers={daq.buffers_read(0)} bytes={daq.bytes_read(0)} "
          f"events={daq.events_decoded(0)} dropped={daq.blocks_dropped(0)}")
daq.stop()

print("\nActive channels:", daq.active_channels(0))
for (board, ch) in daq.active_channels(0):
    qlong = daq.qlong(board, ch)     # numpy uint32 array (65536 bins)
    n = int(qlong.sum())
    peak = int(qlong.argmax())
    print(f"  board {board} ch {ch}: {daq.channel_events(board, ch)} events, "
          f"qlong spectrum sum={n}, peak bin={peak}, nonzero bins={int((qlong>0).sum())}")

print("\nType of a returned spectrum:", type(daq.qlong(0, 0)),
      daq.qlong(0, 0).dtype, daq.qlong(0, 0).shape)
print("Done — files in data/run100/")
