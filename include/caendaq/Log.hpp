#pragma once
//
// Minimal thread-safe logging. Deliberately dependency-free (no log4cplus) so
// the library stays portable. Timestamped lines to stderr, serialized by a
// mutex so interleaved threads don't scramble each other's output.
//
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace caendaq {
namespace detail {

inline std::mutex& logMutex() {
    static std::mutex m;
    return m;
}

inline void logLine(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(logMutex());
    std::clog << '[' << level << "] " << msg << '\n';
}

} // namespace detail
} // namespace caendaq

#define CAENDAQ_LOG(level, expr)                                   \
    do {                                                            \
        std::ostringstream _caendaq_oss;                            \
        _caendaq_oss << expr;                                       \
        ::caendaq::detail::logLine(level, _caendaq_oss.str());      \
    } while (0)

#define LOG_INFO(expr)  CAENDAQ_LOG("INFO ", expr)
#define LOG_WARN(expr)  CAENDAQ_LOG("WARN ", expr)
#define LOG_ERROR(expr) CAENDAQ_LOG("ERROR", expr)
