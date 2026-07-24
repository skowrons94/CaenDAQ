#pragma once
//
// RawBlock — an owning, move-only chunk of raw digitizer bytes.
//
// One block corresponds to the payload returned by a single CAEN ReadData()
// call (the raw board aggregate stream). Blocks are produced by the reader
// thread and consumed by the writer thread through BlockQueue.
//
#include <cstring>
#include <memory>
#include <vector>

namespace caendaq {

class RawBlock {
public:
    RawBlock() = default;

    // Copy `size` bytes from `src` into a freshly owned buffer.
    RawBlock(const char* src, std::size_t size) : data_(size) {
        if (size) std::memcpy(data_.data(), src, size);
    }

    RawBlock(RawBlock&&) noexcept = default;
    RawBlock& operator=(RawBlock&&) noexcept = default;
    RawBlock(const RawBlock&) = delete;
    RawBlock& operator=(const RawBlock&) = delete;

    const char*  data() const noexcept { return data_.data(); }
    char*        data()       noexcept { return data_.data(); }
    std::size_t  size() const noexcept { return data_.size(); }
    bool         empty() const noexcept { return data_.empty(); }

private:
    std::vector<char> data_;
};

// Shared ownership so one readout buffer can fan out to several consumers (the
// disk writer and the decode tap) without copying the payload.
using BlockPtr = std::shared_ptr<const RawBlock>;

} // namespace caendaq
