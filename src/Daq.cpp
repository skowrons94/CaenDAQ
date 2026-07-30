#include "caendaq/Daq.hpp"

#include <stdexcept>

#include "caendaq/Log.hpp"

#ifdef CAENDAQ_WITH_CAEN
#include "caendaq/CaenDigitizer.hpp"
#endif

namespace caendaq {

Daq::Daq(std::string outputDir, std::uint32_t run, Options opt)
    : outputDir_(std::move(outputDir)), run_(run), opt_(std::move(opt)) {}

Daq::~Daq() { stop(); }

std::unique_ptr<IDigitizer> Daq::makeDigitizer(const BoardSpec& spec, std::size_t index) const {
    if (spec.mock) {
        MockDigitizer::Options opt;
        opt.ratePerSec = spec.mockRate;
        opt.waveforms  = spec.mockWaveforms;
        opt.failEvery  = spec.mockFailEvery;
        opt.dpp        = spec.mockDpp;                          // PHA -> energy, PSD -> q
        opt.boardId    = static_cast<std::uint32_t>(index);     // unique id per board
        return std::make_unique<MockDigitizer>(spec.params, opt);
    }
#ifdef CAENDAQ_WITH_CAEN
    return std::make_unique<CaenDigitizer>(spec.params);
#else
    LOG_ERROR("Daq: real board '" << spec.params.name
              << "' requested but this build has no CAEN support "
                 "(rebuild with -DCAENDAQ_WITH_CAEN=ON, or use mock=true)");
    return nullptr;
#endif
}

int Daq::addBoard(const BoardSpec& spec) {
    if (running_ || prepared_) {
        LOG_ERROR("Daq: cannot add a board after prepare()/start()");
        return -1;
    }
    const std::size_t index = runners_.size();
    auto dgtz = makeDigitizer(spec, index);
    if (!dgtz) return -1;

    BoardRunnerConfig cfg;
    cfg.write  = spec.write;
    cfg.decode = spec.decode;
    if (spec.write) anyWrite_ = true;

    // Boards that contribute to the file push to the shared write queue.
    BlockQueue* wq = spec.write ? &writeQueue_ : nullptr;
    runners_.push_back(std::make_unique<BoardRunner>(std::move(dgtz), cfg, wq, &writerFailed_));
    names_.push_back(spec.params.name);
    return static_cast<int>(index);
}

bool Daq::prepare() {
    if (prepared_) return true;
    for (auto& r : runners_) {
        if (!r->prepare()) {
            LOG_ERROR("Daq: prepare failed for board " << r->name());
            return false;
        }
    }

    // Build the single unified writer, its header describing every board in add
    // order (board index i == boardDefs[i]), and open the first file.
    if (anyWrite_) {
        std::vector<XdaqBoardDef> defs;
        defs.reserve(runners_.size());
        for (auto& r : runners_) defs.push_back(boardDefFrom(r->boardInfo()));

        WriterConfig wc;
        wc.directory    = outputDir_;
        wc.runNumber    = run_;
        wc.maxFileBytes = opt_.maxFileBytes;
        wc.writeHeader  = opt_.writeHeader;
        writer_ = std::make_unique<RawFileWriter>(wc);
        writer_->setBoards(std::move(defs));
        if (!writer_->open()) {
            LOG_ERROR("Daq: could not open the output file in '" << outputDir_ << "'");
            writer_.reset();
            return false;
        }
    }

    prepared_ = true;
    return true;
}

bool Daq::start() {
    if (running_) return true;
    if (!prepare()) return false;

    // Start the single writer thread first so it is ready to drain buffers.
    writerFailed_.store(false);
    if (anyWrite_) writerThread_ = std::thread([this] { writerLoop(); });

    // Start every board; on any failure, stop the ones already started + writer.
    for (std::size_t i = 0; i < runners_.size(); ++i) {
        if (!runners_[i]->start()) {
            LOG_ERROR("Daq: start failed for board " << runners_[i]->name());
            for (std::size_t j = 0; j <= i; ++j) runners_[j]->stop();
            teardownWriter();
            return false;
        }
    }
    // Start the statistics/Graphite thread (samples the just-started runners).
    StatsCollector::Options sopt;
    sopt.intervalMs   = opt_.statsIntervalMs;
    sopt.graphiteHost = opt_.graphiteHost;
    sopt.graphitePort = opt_.graphitePort;
    stats_ = std::make_unique<StatsCollector>([this] { return sampleStats(); }, sopt);
    stats_->start();

    running_ = true;
    return true;
}

void Daq::stop() {
    if (!running_) return;
    if (stats_) { stats_->stop(); }
    // Stop the boards first (their readers stop pushing), then tear down the
    // shared writer so it drains everything already queued before closing.
    for (auto& r : runners_) r->stop();
    teardownWriter();
    running_ = false;
}

void Daq::writerLoop() {
    try {
        while (auto block = writeQueue_.pop()) {
            const BlockPtr& b = *block;
            if (!b) continue;
            if (writer_ && !writer_->write(b->data(), b->size())) {
                LOG_ERROR("Daq: file write failed — stopping writer");
                writerFailed_.store(true);
                break;
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Daq: writer thread exception: " << e.what());
        writerFailed_.store(true);
    } catch (...) {
        LOG_ERROR("Daq: writer thread unknown exception");
        writerFailed_.store(true);
    }
}

void Daq::teardownWriter() {
    // Close the queue so the writer drains the remaining blocks and exits.
    writeQueue_.close();
    if (writerThread_.joinable()) writerThread_.join();
    if (writer_) writer_->close();
}

std::vector<BoardSample> Daq::sampleStats() const {
    std::vector<BoardSample> out;
    out.reserve(runners_.size());
    for (const auto& r : runners_) {
        BoardSample bs;
        bs.name         = r->name();
        bs.bytesWritten = r->bytesWritten();
        bs.boardFail    = r->boardFailures();
        HistogramStore& h = r->histograms();
        for (const auto& bc : h.channels()) {
            ChannelSample cs;
            cs.board  = bc.first;
            cs.ch     = bc.second;
            cs.counts = h.counts(bc.first, bc.second);
            bs.channels.push_back(cs);
        }
        out.push_back(std::move(bs));
    }
    return out;
}

std::vector<BoardRate> Daq::stats() const {
    return stats_ ? stats_->snapshot() : std::vector<BoardRate>{};
}

void Daq::setGraphite(const std::string& host, int port) {
    opt_.graphiteHost = host;
    opt_.graphitePort = port;
    if (stats_) stats_->setGraphite(host, port);
}

const std::string& Daq::boardName(int board) const {
    return names_.at(static_cast<std::size_t>(board));
}

HistogramStore& Daq::histograms(int board) {
    return runners_.at(static_cast<std::size_t>(board))->histograms();
}

std::uint64_t Daq::buffersRead(int board) const {
    return runners_.at(static_cast<std::size_t>(board))->buffersRead();
}
std::uint64_t Daq::bytesRead(int board) const {
    return runners_.at(static_cast<std::size_t>(board))->bytesRead();
}
std::uint64_t Daq::bytesWritten(int board) const {
    return runners_.at(static_cast<std::size_t>(board))->bytesWritten();
}
std::uint64_t Daq::blocksDropped(int board) const {
    return runners_.at(static_cast<std::size_t>(board))->blocksDropped();
}
std::uint64_t Daq::commErrors(int board) const {
    return runners_.at(static_cast<std::size_t>(board))->commErrors();
}
std::uint64_t Daq::eventsDecoded(int board) const {
    return runners_.at(static_cast<std::size_t>(board))->eventsDecoded();
}
std::uint64_t Daq::boardFailures(int board) const {
    return runners_.at(static_cast<std::size_t>(board))->boardFailures();
}

} // namespace caendaq
