#include "caendaq/RawFileWriter.hpp"

#include <filesystem>
#include <iomanip>
#include <sstream>

#include "caendaq/DataFileHeader.hpp"
#include "caendaq/Log.hpp"

namespace caendaq {

namespace fs = std::filesystem;

RawFileWriter::RawFileWriter(WriterConfig cfg) : cfg_(std::move(cfg)) {}

RawFileWriter::~RawFileWriter() { close(); }

std::string RawFileWriter::makePath(int cycle) const {
    std::ostringstream oss;
    oss << cfg_.prefix << "_" << cfg_.board
        << "_run" << cfg_.runNumber
        << "_" << std::setw(4) << std::setfill('0') << cycle
        << ".caendat";
    return (fs::path(cfg_.directory) / oss.str()).string();
}

bool RawFileWriter::open() {
    cycle_ = 0;
    totalBytes_ = 0;
    totalBuffers_ = 0;
    headerWritten_ = false;
    return openCycle(/*firstFile=*/true);
}

bool RawFileWriter::openCycle(bool firstFile) {
    std::error_code ec;
    fs::create_directories(cfg_.directory, ec); // no-op if it already exists
    if (ec) {
        LOG_ERROR("RawFileWriter: cannot create directory '" << cfg_.directory
                  << "': " << ec.message());
        return false;
    }

    const std::string path = makePath(cycle_);
    file_.close();
    file_.clear();
    file_.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        LOG_ERROR("RawFileWriter: cannot open '" << path << "' for writing");
        return false;
    }
    fileBytes_ = 0;
    closed_ = false;
    LOG_INFO("RawFileWriter: opened " << path);

    // The XDAQ header is written only to the first file of the run; split files
    // start directly with a board aggregate (RUReader handles both).
    if (firstFile && cfg_.writeHeader) {
        const std::vector<std::uint32_t> header =
            buildXdaqHeader({boardDefFrom(info_)}, nowEpoch32());
        const std::size_t nbytes = header.size() * sizeof(std::uint32_t);
        file_.write(reinterpret_cast<const char*>(header.data()),
                    static_cast<std::streamsize>(nbytes));
        if (!file_) {
            LOG_ERROR("RawFileWriter: failed writing header to '" << path << "'");
            return false;
        }
        fileBytes_ += nbytes; // counts toward the split limit, not payload
        headerWritten_ = true;
    }
    return true;
}

bool RawFileWriter::write(const char* data, std::size_t len) {
    if (closed_) {
        LOG_ERROR("RawFileWriter: write() on a closed writer");
        return false;
    }
    if (len == 0) return true;

    // Rotate before writing if this block would push us past the limit.
    if (cfg_.maxFileBytes != 0 && fileBytes_ + len > cfg_.maxFileBytes && fileBytes_ > 0) {
        ++cycle_;
        if (!openCycle(/*firstFile=*/false)) return false;
    }

    file_.write(data, static_cast<std::streamsize>(len));
    if (!file_) {
        LOG_ERROR("RawFileWriter: write failed on cycle " << cycle_);
        return false;
    }
    fileBytes_    += len;
    totalBytes_   += len;   // raw payload only
    totalBuffers_ += 1;
    return true;
}

void RawFileWriter::close() {
    if (closed_) return;
    if (file_.is_open()) {
        // No trailer: RUReader reads board aggregates until EOF, so nothing may
        // follow the raw payload.
        file_.flush();
        file_.close();
    }
    closed_ = true;
}

} // namespace caendaq
