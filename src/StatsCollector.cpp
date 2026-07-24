#include "caendaq/StatsCollector.hpp"

#include <cctype>
#include <chrono>
#include <ctime>
#include <map>
#include <sstream>

#include "caendaq/Log.hpp"

namespace caendaq {

namespace {
std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return out;
}
std::uint32_t keyOf(std::uint16_t board, std::uint16_t ch) {
    return (static_cast<std::uint32_t>(board) << 16) | ch;
}
} // namespace

StatsCollector::StatsCollector(SampleFn sampler, Options opt)
    : sampler_(std::move(sampler)), opt_(std::move(opt)) {
    if (!opt_.graphiteHost.empty()) {
        graphite_ = std::make_unique<GraphiteClient>(opt_.graphiteHost, opt_.graphitePort);
        LOG_INFO("StatsCollector: Graphite enabled -> " << opt_.graphiteHost << ":" << opt_.graphitePort);
    }
}

StatsCollector::~StatsCollector() { stop(); }

void StatsCollector::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { loop(); });
}

void StatsCollector::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::vector<BoardRate> StatsCollector::snapshot() const {
    std::lock_guard<std::mutex> lk(snapMtx_);
    return latest_;
}

void StatsCollector::loop() {
    using clock = std::chrono::steady_clock;

    std::vector<BoardSample> prev;
    try { prev = sampler_(); } catch (...) {}
    auto prevTime = clock::now();

    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(cvMtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(opt_.intervalMs),
                         [this] { return !running_.load(); });
        }
        if (!running_.load()) break;

        std::vector<BoardSample> cur;
        try { cur = sampler_(); } catch (const std::exception& e) {
            LOG_ERROR("StatsCollector: sampler threw: " << e.what());
            continue;
        } catch (...) { continue; }

        const auto now = clock::now();
        double dt = std::chrono::duration<double>(now - prevTime).count();
        if (dt <= 0) dt = 1e-6;

        // Index the previous sample for differencing.
        std::map<std::string, std::uint64_t> prevBytes;
        std::map<std::string, std::map<std::uint32_t, HistogramStore::Counts>> prevCounts;
        for (const auto& b : prev) {
            prevBytes[b.name] = b.bytesWritten;
            for (const auto& c : b.channels) prevCounts[b.name][keyOf(c.board, c.ch)] = c.counts;
        }

        std::vector<BoardRate> rates;
        rates.reserve(cur.size());
        for (const auto& b : cur) {
            BoardRate br;
            br.name = b.name;
            const std::uint64_t pb = prevBytes.count(b.name) ? prevBytes[b.name] : b.bytesWritten;
            br.writeRate = (b.bytesWritten >= pb) ? (b.bytesWritten - pb) / dt : 0.0;

            auto& pc = prevCounts[b.name];
            for (const auto& c : b.channels) {
                const auto it = pc.find(keyOf(c.board, c.ch));
                const HistogramStore::Counts p = (it != pc.end()) ? it->second : HistogramStore::Counts{};
                ChannelRate cr;
                cr.board  = c.board;
                cr.ch     = c.ch;
                cr.events = (c.counts.events >= p.events) ? (c.counts.events - p.events) / dt : 0.0;
                cr.pileup = (c.counts.pileup >= p.pileup) ? (c.counts.pileup - p.pileup) / dt : 0.0;
                cr.lost   = (c.counts.lost   >= p.lost)   ? (c.counts.lost   - p.lost)   / dt : 0.0;
                cr.satu   = (c.counts.satu   >= p.satu)   ? (c.counts.satu   - p.satu)   / dt : 0.0;
                br.channels.push_back(cr);
            }
            rates.push_back(std::move(br));
        }

        {
            std::lock_guard<std::mutex> lk(snapMtx_);
            latest_ = rates;
        }

        if (graphite_ && graphite_->enabled()) {
            graphite_->send(formatGraphite(rates, static_cast<long long>(std::time(nullptr))));
        }

        prev = std::move(cur);
        prevTime = now;
    }
}

std::string StatsCollector::formatGraphite(const std::vector<BoardRate>& rates, long long epoch) const {
    std::ostringstream oss;
    for (const auto& b : rates) {
        const std::string bn = sanitize(b.name);
        oss << opt_.prefix << '.' << bn << ".writeRate " << b.writeRate << ' ' << epoch << '\n';
        for (const auto& c : b.channels) {
            const std::string base = opt_.prefix + '.' + bn +
                                     ".bo_" + std::to_string(c.board) +
                                     ".ch_" + std::to_string(c.ch) + '.';
            oss << base << "totalRate " << c.events << ' ' << epoch << '\n';
            oss << base << "pileRate "  << c.pileup << ' ' << epoch << '\n';
            oss << base << "lostRate "  << c.lost   << ' ' << epoch << '\n';
            oss << base << "satuRate "  << c.satu   << ' ' << epoch << '\n';
        }
    }
    return oss.str();
}

} // namespace caendaq
