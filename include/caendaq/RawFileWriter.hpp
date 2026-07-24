#pragma once
//
// RawFileWriter — appends raw board buffers to .caendat files, writes the XDAQ
// data-file header on the first file, a trailer at close, and rolls over to a
// new cycle file once a configurable size limit is exceeded.
//
// File naming:  <dir>/<prefix>_<board>_run<run>_<cyc>.caendat
//   e.g. data/run42/ru_V1730A_run42_0000.caendat
// The "ru" prefix + ".caendat" suffix keep it glob-compatible with the existing
// convert.sh (which globs run<N>/ru*.caendat).
//
#include <cstdint>
#include <fstream>
#include <string>

#include "caendaq/BoardInfo.hpp"

namespace caendaq {

struct WriterConfig {
    std::string   directory   = ".";     // output directory (created if missing)
    std::string   prefix      = "ru";     // filename prefix (keep "ru" for RUReader)
    std::string   board       = "board";  // board name, embedded in filename
    std::uint32_t runNumber   = 0;
    std::uint64_t maxFileBytes = 0;        // 0 = never split (single file per run)
    bool          writeHeader  = true;     // write the self-describing header/trailer
};

class RawFileWriter {
public:
    explicit RawFileWriter(WriterConfig cfg);
    ~RawFileWriter();

    RawFileWriter(const RawFileWriter&) = delete;
    RawFileWriter& operator=(const RawFileWriter&) = delete;

    // Board info to embed in the file header. Call after the board is configured
    // and before open(). Optional (mock/unknown boards may leave it default).
    void setBoardInfo(const BoardInfo& info) { info_ = info; }

    // Open the first file and (optionally) write the run header. Returns false
    // on I/O error (never throws).
    bool open();

    // Append `len` bytes of raw board data, rotating to a new file first if the
    // current one would exceed maxFileBytes. Returns false on I/O error.
    bool write(const char* data, std::size_t len);

    // Write the trailer (if header was written) and close. Safe to call twice.
    void close();

    std::uint64_t bytesWritten() const { return totalBytes_; }
    int           currentCycle() const { return cycle_; }

private:
    bool openCycle(bool firstFile);
    std::string makePath(int cycle) const;

    WriterConfig  cfg_;
    BoardInfo     info_;
    std::ofstream file_;
    int           cycle_        = 0;
    std::uint64_t fileBytes_    = 0;   // bytes in the current cycle file
    std::uint64_t totalBytes_   = 0;   // raw payload bytes across the whole run
    std::uint64_t totalBuffers_ = 0;   // number of raw buffers written this run
    bool          headerWritten_ = false;
    bool          closed_        = true;
};

} // namespace caendaq
