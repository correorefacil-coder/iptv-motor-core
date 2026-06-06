#pragma once

#include <string>
#include <vector>
#include <mutex>

enum class LogLevel {
    INFO,
    WARN,
    ERROR
};

class Logger {
public:
    static Logger& GetInstance();

    void Log(LogLevel level, const std::string& message);
    std::vector<std::string> GetLogs(size_t limit = 100);

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex mutex_;
    std::vector<std::string> log_buffer_;
    const size_t max_logs_ = 500;
};

#define LOG_INFO(msg) Logger::GetInstance().Log(LogLevel::INFO, msg)
#define LOG_WARN(msg) Logger::GetInstance().Log(LogLevel::WARN, msg)
#define LOG_ERROR(msg) Logger::GetInstance().Log(LogLevel::ERROR, msg)
