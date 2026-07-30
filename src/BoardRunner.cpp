#include "caendaq/BoardRunner.hpp"

#include <chrono>

#include "caendaq/Log.hpp"

namespace caendaq {

BoardRunner::BoardRunner(std::unique_ptr<IDigitizer> dgtz, BoardRunnerConfig cfg,
                         BlockQueue* writeQueue, std::atomic<bool>* writerFailed)
    : dgtz_(std::move(dgtz)),
      cfg_(std::move(cfg)),
      writeQueue_(writeQueue),
      writerFailed_(writerFailed) {}

BoardRunner::~BoardRunner() { stop(); }

bool BoardRunner::prepare() {
    if (!dgtz_->open())      { LOG_ERROR(name() << ": open failed");      return false; }
    if (!dgtz_->configure()) { LOG_ERROR(name() << ": configure failed"); return false; }
    // Capture the (now known) board identity for the shared file header.
    boardInfo_ = dgtz_->info();

    if (cfg_.decode) {
        decodeStage_ = std::make_unique<DecodeStage>(boardInfo_, histograms_, cfg_.decodeQueueCapacity);
        LOG_INFO(name() << ": parallel decode enabled");
    }
    if (!cfg_.write || writeQueue_ == nullptr) {
        LOG_INFO(name() << ": no-write mode (this board's buffers are not saved)");
    }
    return true;
}

bool BoardRunner::start() {
    if (running_.exchange(true)) return true; // already running
    if (!dgtz_->start()) {
        LOG_ERROR(name() << ": acquisition start failed");
        running_ = false;
        return false;
    }
    LOG_INFO(name() << ": run started");
    return true;
}

void BoardRunner::startReader() {
    if (readerThread_.joinable()) return;   // already up
    if (decodeStage_) decodeStage_->start();
    readerThread_ = std::thread([this] { readerLoop(); });
}

bool BoardRunner::arm() {
    if (running_.exchange(true)) return true; // already armed / running
    if (!dgtz_->arm()) {
        LOG_ERROR(name() << ": could not arm the acquisition");
        running_ = false;
        return false;
    }
    // The reader stays down until the chain is actually started — see
    // startReader(), which explains why bringing it up here deadlocks every
    // board that shares a bridge with the master.
    boardInfo_ = dgtz_->info();   // refresh: arming changed 0x8100
    LOG_INFO(name() << ": armed — waiting for the start signal from the chain");
    return true;
}

void BoardRunner::stop() {
    if (!running_.exchange(false)) return; // not running / already stopped

    // 1. Stop the board so no new data arrives.
    dgtz_->stop();
    // 2. Let the reader finish its current iteration and exit (it stops pushing
    //    to the shared write queue — Daq closes/joins that queue afterwards).
    if (readerThread_.joinable()) readerThread_.join();
    // 3. Stop the decode tap (drains its own queue).
    if (decodeStage_) decodeStage_->stop();
    // 4. Release the board.
    dgtz_->close();

    LOG_INFO(name() << ": run stopped — buffers=" << buffersRead_.load()
             << " bytes=" << bytesRead_.load()
             << " dropped=" << blocksDropped_.load()
             << " commErrors=" << commErrors_.load());
}

void BoardRunner::readerLoop() {
    try {
        const char* data = nullptr;
        std::size_t size = 0;
        const bool writing = cfg_.write && writeQueue_ != nullptr;
        while (running_.load()) {
            if (writerFailed_ && writerFailed_->load()) {
                LOG_ERROR(name() << ": shared writer failed, stopping reader");
                break;
            }
            if (!dgtz_->read(&data, &size)) {
                // Communication error: count it, back off, and let the board
                // wrapper attempt recovery on the next read().
                commErrors_.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(cfg_.reconnectBackoffMs));
                continue;
            }
            if (size == 0) {
                // Nothing pending. Let the other boards on this link have it —
                // see BoardRunnerConfig::idlePollMs.
                if (cfg_.idlePollMs > 0)
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(cfg_.idlePollMs));
                continue;
            }

            buffersRead_.fetch_add(1, std::memory_order_relaxed);
            bytesRead_.fetch_add(size, std::memory_order_relaxed);

            // One shared buffer fans out to the shared file writer (must not
            // drop) and the optional decode tap (best-effort).
            BlockPtr block = std::make_shared<const RawBlock>(data, size);
            if (writing) {
                if (writeQueue_->push(block)) {
                    bytesWritten_.fetch_add(size, std::memory_order_relaxed);
                } else {
                    blocksDropped_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (decodeStage_) decodeStage_->submit(block);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(name() << ": reader thread exception: " << e.what());
    } catch (...) {
        LOG_ERROR(name() << ": reader thread unknown exception");
    }
}

} // namespace caendaq
