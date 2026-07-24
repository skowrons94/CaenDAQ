#pragma once
//
// BlockQueue — a bounded, thread-safe, single-producer / single-consumer queue
// that decouples the (time-critical) board reader thread from the (potentially
// stalling) disk writer thread.
//
// Design choices for robustness:
//   * Bounded capacity so a slow disk can never grow memory without limit.
//   * push() applies back-pressure: it waits up to a timeout for space. If the
//     queue is still full it DROPS the block and increments a counter, rather
//     than blocking the reader forever (which would overflow the board memory).
//     Dropping is counted and surfaced so the operator knows data was lost.
//   * close() wakes every waiter so threads shut down cleanly.
//
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "caendaq/RawBlock.hpp"

namespace caendaq {

class BlockQueue {
public:
    explicit BlockQueue(std::size_t capacity) : capacity_(capacity ? capacity : 1) {}

    // Producer side. Returns true if enqueued, false if dropped (queue full)
    // or the queue is closed. Waits at most `wait` for room (pass 0ms for a
    // best-effort tap that drops immediately rather than back-pressuring).
    bool push(BlockPtr block, std::chrono::milliseconds wait = std::chrono::milliseconds(100)) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (closed_) return false;
        if (q_.size() >= capacity_) {
            not_full_.wait_for(lk, wait, [&] { return closed_ || q_.size() < capacity_; });
            if (closed_) return false;
            if (q_.size() >= capacity_) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                dropped_bytes_.fetch_add(block ? block->size() : 0, std::memory_order_relaxed);
                return false;
            }
        }
        q_.push_back(std::move(block));
        not_empty_.notify_one();
        return true;
    }

    // Consumer side. Blocks until a block is available or the queue is closed
    // and drained. Returns std::nullopt only when closed and empty.
    std::optional<BlockPtr> pop() {
        std::unique_lock<std::mutex> lk(mtx_);
        not_empty_.wait(lk, [&] { return closed_ || !q_.empty(); });
        if (q_.empty()) return std::nullopt; // closed and drained
        BlockPtr b = std::move(q_.front());
        q_.pop_front();
        not_full_.notify_one();
        return b;
    }

    // Signal shutdown: no more pushes accepted; pop() drains then returns nullopt.
    void close() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    std::size_t   size() const { std::lock_guard<std::mutex> lk(mtx_); return q_.size(); }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t droppedBytes() const { return dropped_bytes_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex        mtx_;
    std::condition_variable   not_empty_;
    std::condition_variable   not_full_;
    std::deque<BlockPtr>      q_;
    const std::size_t         capacity_;
    bool                      closed_ = false;
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> dropped_bytes_{0};
};

} // namespace caendaq
