#ifndef NON_BLOCKING_LOGGER_H
#define NON_BLOCKING_LOGGER_H

#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <fstream>
#include <sstream>
#include <memory>
#include <atomic>

namespace AES67 {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3,
    CRITICAL = 4
};

class NonBlockingLogger {
public:
    explicit NonBlockingLogger(const std::string& filename = "/tmp/aes67_driver.log");
    ~NonBlockingLogger();
    
    // Log a message with specified level
    void log(LogLevel level, const std::string& message);
    
    // Convenience methods for different log levels
    void debug(const std::string& message);
    void info(const std::string& message);
    void warning(const std::string& message);
    void error(const std::string& message);
    void critical(const std::string& message);
    
    // Set minimum log level (messages below this level will be ignored)
    void setLogLevel(LogLevel level);
    
    // Flush all pending messages to disk
    void flush();
    
private:
    struct LogEntry {
        LogLevel level;
        std::string message;
        std::string timestamp;
        
        LogEntry(LogLevel l, const std::string& msg);
    };
    
    void writerThreadFunc();
    
    std::queue<LogEntry> logQueue_;
    mutable std::mutex queueMutex_;
    std::thread writerThread_;
    std::ofstream logFile_;
    std::atomic<bool> shouldStop_{false};
    std::atomic<LogLevel> minLogLevel_{LogLevel::INFO};
    
    // Level names for formatting
    static const char* levelToString(LogLevel level);
};

// Global logger instance
extern std::unique_ptr<NonBlockingLogger> g_logger;

// Macros for easy logging.
//
// Each expands to a single statement (do { ... } while (0)). A bare
// "if (g_logger) ..." would capture a following else:
//     if (x) LOG_INFO("a"); else other();   // else would bind to the macro's if
#define LOG_DEBUG(msg)    do { if (AES67::g_logger) { AES67::g_logger->debug(msg); } } while (0)
#define LOG_INFO(msg)     do { if (AES67::g_logger) { AES67::g_logger->info(msg); } } while (0)
#define LOG_WARNING(msg)  do { if (AES67::g_logger) { AES67::g_logger->warning(msg); } } while (0)
#define LOG_ERROR(msg)    do { if (AES67::g_logger) { AES67::g_logger->error(msg); } } while (0)
#define LOG_CRITICAL(msg) do { if (AES67::g_logger) { AES67::g_logger->critical(msg); } } while (0)

} // namespace AES67

#endif // NON_BLOCKING_LOGGER_H