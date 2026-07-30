#include "caendaq/StatsCollector.hpp"

#include <cctype>
#include <chrono>
#include <ctime>
#include <map>
#include <sstream>

#include "caendaq/Log.hpp"

namespace caendaq {

namespace {
// A metric prefix is a dotted path ("ancillary.rates.12c12c"), so the dot has to
// survive; everything else Graphite would choke on becomes '_'. Empty and
// dot-only prefixes fall back to the default so a mistyped setting cannot
// produce paths starting with '.'.
std::string sanitizePrefix(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
    }
    // Trim leading/trailing dots, which would give empty path segments.
    const auto first = out.find_first_not_of('.');
    if (first == std::string::npos) return "ancillary.rates";
    const auto last = out.find_last_not_of('.');
    return out.substr(first, last - first + 1);
}
std::uint32_t keyOf(std::uint16_t board, std::uint16_t ch) {
    return (static_cast<std::uint32_t>(board) << 16) | ch;
}
} // namespace

StatsCollector::StatsCollector(SampleFn sampler, Options opt)
    : sampler_(std::move(sampler)), opt_(std::move(opt)) {
    opt_.prefix = sanitizePrefix(opt_.prefix);
    if (!opt_.graphiteHost.empty()) {
        graphite_ = std::make_unique<GraphiteClient>(opt_.graphiteHost, opt_.graphitePort);
        LOG_INFO("StatsCollector: Graphite enabled -> " << opt_.graphiteHost << ":"
                 << opt_.graphitePort << " under '" << opt_.prefix << "'");
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

void StatsCollector::setGraphite(const std::string& host, int port, const std::string& prefix) {
    std::lock_guard<std::mutex> lk(graphiteMtx_);
    if (!prefix.empty()) opt_.prefix = sanitizePrefix(prefix);
    if (host.empty()) {
        graphite_.reset();
        LOG_INFO("StatsCollector: Graphite disabled");
    } else {
        graphite_ = std::make_unique<GraphiteClient>(host, port);
        LOG_INFO("StatsCollector: Graphite target -> " << host << ":" << port
                 << " under '" << opt_.prefix << "'");
    }
    opt_.graphiteHost = host;
    opt_.graphitePort = port;
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

        // Index the previous sample for differencing. Keyed by the VME board id,
        // not the name: two digitizers of the same model share a name, and
        // keying on that would difference one board against the other.
        std::map<std::uint16_t, std::uint64_t> prevBytes;
        std::map<std::uint16_t, std::map<std::uint32_t, HistogramStore::Counts>> prevCounts;
        for (const auto& b : prev) {
            prevBytes[b.boardRegId] = b.bytesWritten;
            for (const auto& c : b.channels) prevCounts[b.boardRegId][keyOf(c.board, c.ch)] = c.counts;
        }

        std::vector<BoardRate> rates;
        rates.reserve(cur.size());
        for (const auto& b : cur) {
            BoardRate br;
            br.name = b.name;
            br.boardRegId = b.boardRegId;
            br.boardFail = b.boardFail; // cumulative, passed through (not a rate)
            const std::uint64_t pb = prevBytes.count(b.boardRegId) ? prevBytes[b.boardRegId]
                                                                   : b.bytesWritten;
            br.writeRate = (b.bytesWritten >= pb) ? (b.bytesWritten - pb) / dt : 0.0;

            auto& pc = prevCounts[b.boardRegId];
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

        {
            std::lock_guard<std::mutex> lk(graphiteMtx_);
            if (graphite_ && graphite_->enabled()) {
                graphite_->send(formatGraphite(rates, static_cast<long long>(std::time(nullptr))));
            }
        }

        prev = std::move(cur);
        prevTime = now;
    }
}

// <prefix>.bo_<board>.writeRate and <prefix>.bo_<board>.ch_<ch>.<metric>.
//
// The board level is the VME board id, not the model name: the name comes from
// the board's configuration and two digitizers of the same model carry the same
// one, which would silently merge their series. The prefix identifies the
// EXPERIMENT ('ancillary.rates.12c12c'), so a campaign owns one subtree.
std::string StatsCollector::formatGraphite(const std::vector<BoardRate>& rates, long long epoch) const {
    std::ostringstream oss;
    for (const auto& b : rates) {
        const std::string bo = opt_.prefix + ".bo_" + std::to_string(b.boardRegId);
        oss << bo << ".writeRate " << b.writeRate << ' ' << epoch << '\n';
        for (const auto& c : b.channels) {
            const std::string base = opt_.prefix +
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
