#include "caendaq/BoardRunner.hpp"

#include <chrono>

#include "caendaq/Log.hpp"

namespace caendaq {

BoardRunner::BoardRunner(std::unique_ptr<IDigitizer> dgtz, BoardRunnerConfig cfg)
    : dgtz_(std::move(dgtz)),
      cfg_(std::move(cfg)),
      writer_(cfg_.writer),
      queue_(cfg_.queueCapacity) {}

BoardRunner::~BoardRunner() { stop(); }

bool BoardRunner::prepare() {
    if (!dgtz_->open())      { LOG_ERROR(name() << ": open failed");      return false; }
    if (!dgtz_->configure()) { LOG_ERROR(name() << ": configure failed"); return false; }
    // Embed the (now known) board identity in the output file header.
    const BoardInfo info = dgtz_->info();
    writer_.setBoardInfo(info);
    if (cfg_.write) {
        if (!writer_.open()) { LOG_ERROR(name() << ": output file open failed"); return false; }
    } else {
        LOG_INFO(name() << ": no-write mode (decode-only, no .caendat files)");
    }

    if (cfg_.decode) {
        decodeStage_ = std::make_unique<DecodeStage>(info, histograms_, cfg_.decodeQueueCapacity);
        LOG_INFO(name() << ": parallel decode enabled");
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
    if (decodeStage_) decodeStage_->start();
    if (cfg_.write) writerThread_ = std::thread([this] { writerLoop(); });
    readerThread_ = std::thread([this] { readerLoop(); });
    LOG_INFO(name() << ": run started");
    return true;
}

void BoardRunner::stop() {
    if (!running_.exchange(false)) return; // not running / already stopped

    // 1. Stop the board so no new data arrives.
    dgtz_->stop();
    // 2. Let the reader finish its current iteration and exit.
    if (readerThread_.joinable()) readerThread_.join();
    // 3. Close the queue so the writer drains remaining blocks then exits.
    queue_.close();
    if (writerThread_.joinable()) writerThread_.join();
    // 4. Stop the decode tap (drains its own queue).
    if (decodeStage_) decodeStage_->stop();
    // 5. Flush + close file (no-op if never opened), release the board.
    if (cfg_.write) writer_.close();
    dgtz_->close();

    LOG_INFO(name() << ": run stopped — buffers=" << buffersRead_.load()
             << " bytes=" << bytesRead_.load()
             << " dropped=" << queue_.dropped()
             << " commErrors=" << commErrors_.load());
}

void BoardRunner::readerLoop() {
    try {
        const char* data = nullptr;
        std::size_t size = 0;
        while (running_.load()) {
            if (writerFailed_.load()) {
                LOG_ERROR(name() << ": writer failed, stopping reader");
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
            if (size == 0) continue; // no data available right now

            buffersRead_.fetch_add(1, std::memory_order_relaxed);
            bytesRead_.fetch_add(size, std::memory_order_relaxed);

            // One shared buffer fans out to the writer (must not drop) and the
            // optional decode tap (best-effort).
            BlockPtr block = std::make_shared<const RawBlock>(data, size);
            if (cfg_.write) queue_.push(block);
            if (decodeStage_) decodeStage_->submit(block);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(name() << ": reader thread exception: " << e.what());
    } catch (...) {
        LOG_ERROR(name() << ": reader thread unknown exception");
    }
}

void BoardRunner::writerLoop() {
    try {
        while (auto block = queue_.pop()) {
            const BlockPtr& b = *block;
            if (!b) continue;
            if (!writer_.write(b->data(), b->size())) {
                writerFailed_.store(true);
                break;
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(name() << ": writer thread exception: " << e.what());
        writerFailed_.store(true);
    } catch (...) {
        LOG_ERROR(name() << ": writer thread unknown exception");
        writerFailed_.store(true);
    }
}

} // namespace caendaq
