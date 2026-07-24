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

std::unique_ptr<IDigitizer> Daq::makeDigitizer(const BoardSpec& spec) const {
    if (spec.mock) {
        MockDigitizer::Options opt;
        opt.ratePerSec = spec.mockRate;
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
    auto dgtz = makeDigitizer(spec);
    if (!dgtz) return -1;

    BoardRunnerConfig cfg;
    cfg.writer.directory    = outputDir_;
    cfg.writer.prefix       = "ru";
    cfg.writer.board        = spec.params.name;
    cfg.writer.runNumber    = run_;
    cfg.writer.maxFileBytes = opt_.maxFileBytes;
    cfg.writer.writeHeader  = opt_.writeHeader;
    cfg.write               = spec.write;
    cfg.decode              = spec.decode;

    runners_.push_back(std::make_unique<BoardRunner>(std::move(dgtz), cfg));
    names_.push_back(spec.params.name);
    return static_cast<int>(runners_.size() - 1);
}

bool Daq::prepare() {
    if (prepared_) return true;
    for (auto& r : runners_) {
        if (!r->prepare()) {
            LOG_ERROR("Daq: prepare failed for board " << r->name());
            return false;
        }
    }
    prepared_ = true;
    return true;
}

bool Daq::start() {
    if (running_) return true;
    if (!prepare()) return false;
    // Start every board; on any failure, stop the ones already started.
    for (std::size_t i = 0; i < runners_.size(); ++i) {
        if (!runners_[i]->start()) {
            LOG_ERROR("Daq: start failed for board " << runners_[i]->name());
            for (std::size_t j = 0; j <= i; ++j) runners_[j]->stop();
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
    for (auto& r : runners_) r->stop();
    running_ = false;
}

std::vector<BoardSample> Daq::sampleStats() const {
    std::vector<BoardSample> out;
    out.reserve(runners_.size());
    for (const auto& r : runners_) {
        BoardSample bs;
        bs.name         = r->name();
        bs.bytesWritten = r->bytesWritten();
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

} // namespace caendaq
