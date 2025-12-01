#include "logger.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>

namespace logging {
namespace {
std::atomic<int> g_log_level{0};
std::mutex g_log_mutex;
auto g_start_time = std::chrono::steady_clock::now();

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Debug:
            return "DEBUG";
    }
    return "UNKNOWN";
}
}

void InitLogger(int level) {
    g_start_time = std::chrono::steady_clock::now();
    SetLogLevel(level);
}

void SetLogLevel(int level) {
    int clamped = std::max(0, std::min(3, level));
    g_log_level.store(clamped, std::memory_order_relaxed);
}

int GetLogLevel() {
    return g_log_level.load(std::memory_order_relaxed);
}

bool ShouldLog(LogLevel level) {
    return static_cast<int>(level) <= g_log_level.load(std::memory_order_relaxed);
}

void LogMessage(LogLevel level, const char* tag, const std::string& message) {
    if (!ShouldLog(level)) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    auto since_start = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_start_time).count();

    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ostream& out = (level == LogLevel::Error) ? std::cerr : std::cout;
    out << '[' << since_start << "ms] " << LevelName(level) << ' ' << tag << ": " << message << std::endl;
}

ScopedLogTimer::ScopedLogTimer(const char* tag, const char* label, LogLevel level)
    : tag_(tag), label_(label), level_(level), start_(std::chrono::steady_clock::now()) {}

ScopedLogTimer::~ScopedLogTimer() {
    if (!ShouldLog(level_)) {
        return;
    }
    auto end = std::chrono::steady_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
    LogMessage(level_, tag_, std::string(label_) + " took " + std::to_string(duration_us) + "us");
}

}  // namespace logging
