#include "caendaq/DecodeStage.hpp"

#include <chrono>

#include "caendaq/Log.hpp"

namespace caendaq {

DecodeStage::DecodeStage(const BoardInfo& info, HistogramStore& store, HitTap& hits,
                         std::size_t queueCapacity)
    : store_(store), hits_(hits), queue_(queueCapacity) {
    decoder_.registerBoard(info);
}

void DecodeStage::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { loop(); });
}

void DecodeStage::submit(const BlockPtr& block) {
    // Zero wait: if the decoder is behind, drop immediately (counted) rather than
    // back-pressuring the reader. Data is still safe on disk via the writer path.
    queue_.push(block, std::chrono::milliseconds(0));
}

void DecodeStage::stop() {
    if (!running_.exchange(false)) return;
    queue_.close();
    if (thread_.joinable()) thread_.join();
}

void DecodeStage::loop() {
    try {
        while (auto block = queue_.pop()) {
            const BlockPtr& b = *block;
            if (!b) continue;
            const bool tapping = hits_.enabled();
            batch_.clear();
            const std::size_t n =
                decoder_.decode(b->data(), b->size(), [this, tapping](const DecodedEvent& e) {
                    store_.fill(e);
                    // A hit without a time (no time stamp recorded, or a
                    // board whose clock is unknown) cannot be in coincidence.
                    if (tapping && e.timePs > 0) batch_.push_back(HitTap::fromEvent(e));
                });
            if (tapping) hits_.append(batch_);
            events_.fetch_add(n, std::memory_order_relaxed);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("DecodeStage: exception: " << e.what());
    } catch (...) {
        LOG_ERROR("DecodeStage: unknown exception");
    }
}

} // namespace caendaq
