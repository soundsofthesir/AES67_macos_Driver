#include "NonBlockingLogger.h"
#include "PortableUtil.h"
#include <chrono>
#include <iomanip>
#include <ctime>

namespace AES67 {

// Global logger instance
std::unique_ptr<NonBlockingLogger> g_logger;

NonBlockingLogger::LogEntry::LogEntry(LogLevel l, const std::string& msg) 
    : level(l), message(msg) {
    // Create timestamp
    auto now = std::chrono::system_clock::now();
    // (not named time_t: that would shadow the type)
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    // std::localtime returns a shared static std::tm that concurrent callers
    // overwrite; convert into a buffer owned by this call instead
    std::tm localTm{};
    std::ostringstream oss;
    if (aes67::util::safeLocalTime(nowTime, localTm)) {
        oss << std::put_time(&localTm, "%Y-%m-%d %H:%M:%S");
    } else {
        oss << "0000-00-00 00:00:00";
    }
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    timestamp = oss.str();
}

NonBlockingLogger::NonBlockingLogger(const std::string& filename) {
    logFile_.open(filename, std::ios::app);
    if (!logFile_.is_open()) {
        throw std::runtime_error("Could not open log file: " + filename);
    }
    
    // Start the writer thread
    writerThread_ = std::thread(&NonBlockingLogger::writerThreadFunc, this);
}

NonBlockingLogger::~NonBlockingLogger() {
    // Signal the writer thread to stop
    shouldStop_ = true;
    
    // Wake up the writer thread if it's sleeping
    // In a real implementation, we might use a condition variable
    
    if (writerThread_.joinable()) {
        writerThread_.join();
    }
    
    // Flush any remaining messages
    flush();
    
    if (logFile_.is_open()) {
        logFile_.close();
    }
}

void NonBlockingLogger::log(LogLevel level, const std::string& message) {
    // Check if the message should be logged based on the minimum level
    if (level < minLogLevel_.load()) {
        return;
    }
    
    // Create a log entry
    LogEntry entry(level, message);
    
    // Add to the queue (non-blocking operation)
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        logQueue_.emplace(std::move(entry));
    }
}

void NonBlockingLogger::debug(const std::string& message) {
    log(LogLevel::DEBUG, message);
}

void NonBlockingLogger::info(const std::string& message) {
    log(LogLevel::INFO, message);
}

void NonBlockingLogger::warning(const std::string& message) {
    log(LogLevel::WARNING, message);
}

void NonBlockingLogger::error(const std::string& message) {
    log(LogLevel::ERROR, message);
}

void NonBlockingLogger::critical(const std::string& message) {
    log(LogLevel::CRITICAL, message);
}

void NonBlockingLogger::setLogLevel(LogLevel level) {
    minLogLevel_.store(level);
}

void NonBlockingLogger::flush() {
    // Process all remaining entries in the queue
    std::queue<LogEntry> localQueue;
    
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        localQueue.swap(logQueue_);
    }
    
    // Write all entries to the file
    while (!localQueue.empty()) {
        const auto& entry = localQueue.front();
        logFile_ << "[" << entry.timestamp << "] [" 
                 << levelToString(entry.level) << "] " 
                 << entry.message << std::endl;
        localQueue.pop();
    }
    
    logFile_.flush();
}

void NonBlockingLogger::writerThreadFunc() {
    while (!shouldStop_.load()) {
        // Process all available entries in the queue
        std::queue<LogEntry> localQueue;
        
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (!logQueue_.empty()) {
                localQueue.swap(logQueue_);
            }
        }
        
        // Write all entries to the file
        while (!localQueue.empty()) {
            const auto& entry = localQueue.front();
            logFile_ << "[" << entry.timestamp << "] [" 
                     << levelToString(entry.level) << "] " 
                     << entry.message << std::endl;
            localQueue.pop();
        }
        
        logFile_.flush();
        
        // Sleep briefly to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // After stopping, flush any remaining entries
    flush();
}

const char* NonBlockingLogger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:    return "DEBUG";
        case LogLevel::INFO:     return "INFO";
        case LogLevel::WARNING:  return "WARNING";
        case LogLevel::ERROR:    return "ERROR";
        case LogLevel::CRITICAL: return "CRITICAL";
        default:                 return "UNKNOWN";
    }
}

} // namespace AES67