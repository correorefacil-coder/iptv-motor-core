#include "logger.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <sstream>

Logger& Logger::GetInstance() {
    static Logger instance;
    return instance;
}

static std::string GetCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static std::string LogLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

void Logger::Log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string timestamp = GetCurrentTimestamp();
    std::string level_str = LogLevelToString(level);
    std::string formatted_log = "[" + timestamp + "] [" + level_str + "] " + message;
    
    // Print to stdout
    if (level == LogLevel::ERROR) {
        std::cerr << formatted_log << std::endl;
    } else {
        std::cout << formatted_log << std::endl;
    }
    
    // Store in ring buffer
    log_buffer_.push_back(formatted_log);
    if (log_buffer_.size() > max_logs_) {
        log_buffer_.erase(log_buffer_.begin());
    }
}

std::vector<std::string> Logger::GetLogs(size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (limit >= log_buffer_.size()) {
        return log_buffer_;
    }
    return std::vector<std::string>(log_buffer_.end() - limit, log_buffer_.end());
}
