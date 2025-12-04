#ifndef LOGGER_H
#define LOGGER_H

#include <chrono>
#include <sstream>
#include <string>

namespace logging {

enum class LogLevel : int {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3
};

void InitLogger(int level);
void SetLogLevel(int level);
int GetLogLevel();
bool ShouldLog(LogLevel level);
void LogMessage(LogLevel level, const char* tag, const std::string& message);

class ScopedLogTimer {
public:
    ScopedLogTimer(const char* tag, const char* label, LogLevel level = LogLevel::Debug);
    ~ScopedLogTimer();
    ScopedLogTimer(const ScopedLogTimer&) = delete;
    ScopedLogTimer& operator=(const ScopedLogTimer&) = delete;
private:
    const char* tag_;
    const char* label_;
    LogLevel level_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace logging

#define LOG_STREAM(level, tag, stream_expr)                                      \
    do {                                                                         \
        if (::logging::ShouldLog(level)) {                                       \
            std::ostringstream log_stream__;                                     \
            log_stream__ << stream_expr;                                         \
            ::logging::LogMessage(level, tag, log_stream__.str());               \
        }                                                                        \
    } while (0)

#define LOG_ERROR(tag, stream_expr) LOG_STREAM(::logging::LogLevel::Error, tag, stream_expr)
#define LOG_WARN(tag, stream_expr)  LOG_STREAM(::logging::LogLevel::Warn,  tag, stream_expr)
#define LOG_INFO(tag, stream_expr)  LOG_STREAM(::logging::LogLevel::Info,  tag, stream_expr)
#define LOG_DEBUG(tag, stream_expr) LOG_STREAM(::logging::LogLevel::Debug, tag, stream_expr)

#endif  // LOGGER_H
