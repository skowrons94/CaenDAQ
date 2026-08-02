#include "caendaq/Daq.hpp"

#include <algorithm>
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
        opt.startMode  = spec.mockStartMode;                    // mirror the configured chain
        return std::make_unique<MockDigitizer>(spec.params, opt);
    }
#ifdef CAENDAQ_WITH_CAEN
    return std::make_unique<CaenDigitizer>(spec.params);
#else
    // Not a board-model limitation: every model goes through this same branch.
    // The module simply was not compiled against libCAENDigitizer.
    LOG_ERROR("Daq: real board '" << spec.params.name
              << "' requested but this build of CaenDAQ has no CAEN support "
                 "(applies to every model, not just this one). Check "
                 "caendaq.HAS_CAEN: if it is False, the module was built "
                 "without libCAENDigitizer/jsoncpp — install them and "
                 "reinstall (the build auto-detects them; use "
                 "CAENDAQ_WITH_CAEN=ON to make a missing install a hard error "
                 "instead of a silent mock-only build). To run without "
                 "hardware, use mock=true / TEST_FLAG=True");
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

int Daq::masterIndex() const {
    // The master is the board that fires the software trigger starting the
    // chain. CAEN convention (and the previous XDAQ implementation) is board
    // register id 0; fall back to the first synchronised board when no board
    // reports id 0, so an unusual id assignment still starts the run.
    int firstSynced = -1;
    for (std::size_t i = 0; i < runners_.size(); ++i) {
        if (!runners_[i]->synchronised()) continue;
        if (firstSynced < 0) firstSynced = static_cast<int>(i);
        if (runners_[i]->boardInfo().boardRegId == 0) return static_cast<int>(i);
    }
    return firstSynced;
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

    // How each board starts is decided by its OWN configuration — Acquisition
    // Control (0x8100) bits[1:0], as set in the board's register dump:
    //
    //   SW controlled        -> software start; the board runs immediately
    //   first trigger / S-IN -> ARM it; it starts when the external signal comes
    //
    // Every synchronised board must be armed BEFORE the start signal is
    // generated, or it misses the edge and begins late with a different time
    // origin. So: arm/start everything first, then fire the software trigger on
    // the master, which propagates on TRG-OUT into the next board's TRG-IN and
    // so on down the chain.
    const int master = masterIndex();
    if (master >= 0) {
        LOG_INFO("Daq: synchronised start — arming "
                 << std::count_if(runners_.begin(), runners_.end(),
                                  [](const std::unique_ptr<BoardRunner>& r) { return r->synchronised(); })
                 << " board(s); board " << runners_[master]->name()
                 << " (register id " << runners_[master]->boardInfo().boardRegId
                 << ") will fire the software trigger that starts the chain");
    }

    for (std::size_t i = 0; i < runners_.size(); ++i) {
        const bool synced = runners_[i]->synchronised();
        if (!(synced ? runners_[i]->arm() : runners_[i]->start())) {
            LOG_ERROR("Daq: start failed for board " << runners_[i]->name());
            for (std::size_t j = 0; j <= i; ++j) runners_[j]->stop();
            teardownWriter();
            return false;
        }
        // Record the role the board actually ended up with, so run metadata
        // states how the data was taken rather than how it was requested. Set
        // after arming, which refreshes the board info from the hardware.
        runners_[i]->setSyncRole(
            !synced                          ? SyncRole::Independent
            : static_cast<int>(i) == master  ? SyncRole::Master
                                             : SyncRole::Slave);
    }

    // All boards are now armed — safe to release the start signal. No reader is
    // running yet, deliberately: an armed board has no data, so its blocking
    // MBLT read would sit in the driver holding the lock that serialises the
    // link, and the master could never get the link to send this trigger. Every
    // board on one bridge would deadlock. See BoardRunner::startReader().
    if (master >= 0 && !runners_[master]->sendSWTrigger()) {
        LOG_ERROR("Daq: the software trigger failed on master board "
                  << runners_[master]->name()
                  << " — the synchronised boards will never start");
        for (auto& r : runners_) r->stop();
        teardownWriter();
        return false;
    }

    // The chain is going: bring the readout up.
    for (auto& r : runners_) r->startReader();
    // Start the statistics/Graphite thread (samples the just-started runners).
    StatsCollector::Options sopt;
    sopt.intervalMs      = opt_.statsIntervalMs;
    sopt.firstIntervalMs = opt_.statsFirstIntervalMs;
    sopt.graphiteHost = opt_.graphiteHost;
    sopt.graphitePort = opt_.graphitePort;
    if (!opt_.graphitePrefix.empty()) sopt.prefix = opt_.graphitePrefix;
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
    //
    // In a daisy chain this is the mirror image of start(): the master goes
    // first, so deasserting RUN stops every slave at the same instant; the
    // explicit per-slave stop that follows is then just a clean disarm.
    for (auto& r : runners_) r->stop();
    teardownWriter();
    running_ = false;
}

BoardInfo Daq::boardInfo(int board) const {
    const auto i = static_cast<std::size_t>(board);
    if (board < 0 || i >= runners_.size()) return BoardInfo{};
    return runners_[i]->boardInfo();
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
        bs.boardRegId   = static_cast<std::uint16_t>(r->boardInfo().boardRegId);
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

void Daq::setGraphite(const std::string& host, int port, const std::string& prefix) {
    opt_.graphiteHost = host;
    opt_.graphitePort = port;
    if (!prefix.empty()) opt_.graphitePrefix = prefix;
    if (stats_) stats_->setGraphite(host, port, prefix);
}

void Daq::setStatsInterval(int ms) {
    // Remembered on Options too, so a collector built later (a subsequent run on
    // this Daq) starts at the interval the operator last chose.
    // Clamped here too: setStatsInterval() may be called before a run exists,
    // and statsIntervalMs() would otherwise report a value the collector will
    // never actually use.
    opt_.statsIntervalMs = StatsCollector::clampInterval(ms);
    if (stats_) stats_->setInterval(opt_.statsIntervalMs);
}

int Daq::statsIntervalMs() const {
    return stats_ ? stats_->intervalMs() : opt_.statsIntervalMs;
}

bool Daq::writeRegister(int board, std::uint32_t address, std::uint32_t value) {
    if (board < 0 || static_cast<std::size_t>(board) >= runners_.size()) {
        LOG_ERROR("writeRegister: no board with index " << board);
        return false;
    }
    return runners_[static_cast<std::size_t>(board)]->writeRegister(address, value);
}

bool Daq::readRegister(int board, std::uint32_t address, std::uint32_t* value) {
    if (board < 0 || static_cast<std::size_t>(board) >= runners_.size()) {
        LOG_ERROR("readRegister: no board with index " << board);
        return false;
    }
    return runners_[static_cast<std::size_t>(board)]->readRegister(address, value);
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
