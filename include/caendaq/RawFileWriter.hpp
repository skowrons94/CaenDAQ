#pragma once
//
// RawFileWriter — appends the raw buffers of ALL boards to ONE unified .caendat
// stream (each CAEN aggregate self-identifies its board), writes the XDAQ
// data-file header — describing every board — on the first file, and rolls over
// to a new part file once a configurable size limit is exceeded. This mirrors
// the XDAQ ReadoutUnit, which reads a vector of digitizers into a single file.
//
// File naming:  <dir>/run_<run>_<part>.caendat
//   e.g. data/run42/run_42_0000.caendat
// Still ".caendat" so RUReader / convert globs (run*.caendat) match. Before
// creating any part file, if the name already exists a "_1", "_2", ... suffix
// is appended so an existing file is never overwritten.
//
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "caendaq/DataFileHeader.hpp"   // XdaqBoardDef (pulls in BoardInfo)

namespace caendaq {

struct WriterConfig {
    std::string   directory   = ".";     // output directory (created if missing)
    std::uint32_t runNumber   = 0;
    std::uint64_t maxFileBytes = 0;        // 0 = never split (single file per run)
    bool          writeHeader  = true;     // write the self-describing header
};

class RawFileWriter {
public:
    explicit RawFileWriter(WriterConfig cfg);
    ~RawFileWriter();

    RawFileWriter(const RawFileWriter&) = delete;
    RawFileWriter& operator=(const RawFileWriter&) = delete;

    // Board-definition words to embed in the file header, one per board in add
    // order (board index i is boardDefs[i]). Call before open(). Optional (an
    // empty list writes a header with nbBoards = 0).
    void setBoards(std::vector<XdaqBoardDef> boards) { boardDefs_ = std::move(boards); }

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
    // Return `intended` if free, else the same name with "_1"/"_2"/... inserted
    // before ".caendat" — so an existing file is never truncated.
    std::string uniquePath(const std::string& intended) const;

    WriterConfig  cfg_;
    std::vector<XdaqBoardDef> boardDefs_;   // header board table (add order)
    std::ofstream file_;
    int           cycle_        = 0;
    std::uint64_t fileBytes_    = 0;   // bytes in the current cycle file
    std::uint64_t totalBytes_   = 0;   // raw payload bytes across the whole run
    std::uint64_t totalBuffers_ = 0;   // number of raw buffers written this run
    bool          headerWritten_ = false;
    bool          closed_        = true;
};

} // namespace caendaq
