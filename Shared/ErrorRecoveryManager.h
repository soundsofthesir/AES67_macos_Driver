#ifndef ERROR_RECOVERY_MANAGER_H
#define ERROR_RECOVERY_MANAGER_H

#include <atomic>
#include <functional>
#include <chrono>
#include <cstddef>
#include <unordered_map>
#include <string>

namespace AES67 {

/**
 * Manages error recovery for the AES67 driver
 * 
 * Automatically attempts recovery when errors occur
 */
class ErrorRecoveryManager {
public:
    enum class ErrorType {
        PTP_SYNC_LOSS,
        NETWORK_DISCONNECT,
        BUFFER_UNDERRUN,
        BUFFER_OVERRUN,
        STREAM_MISCONFIGURED,
        DEVICE_UNRESPONSIVE
    };
    
    using RecoveryAction = std::function<bool()>;
    
    ErrorRecoveryManager();
    ~ErrorRecoveryManager();
    
    /**
     * Register a recovery action for a specific error type
     * @param errorType Type of error
     * @param action Function to execute for recovery
     */
    void registerRecoveryAction(ErrorType errorType, RecoveryAction action);
    
    /**
     * Report an error and trigger recovery if needed
     * @param errorType Type of error that occurred
     * @param context Additional context about the error
     */
    void reportError(ErrorType errorType, const std::string& context = "");
    
    /**
     * Get the time since last error
     */
    std::chrono::steady_clock::duration getTimeSinceLastError() const;
    
    /**
     * Check if system is in recovery mode
     */
    bool isInRecovery() const;
    
    /**
     * Get recovery attempt count
     */
    size_t getRecoveryAttempts() const;
    
    /**
     * Reset recovery statistics
     */
    void resetStatistics();
    
    /**
     * Enable/disable automatic recovery
     */
    void setAutoRecoveryEnabled(bool enabled);
    bool isAutoRecoveryEnabled() const;

private:
    struct RecoveryState {
        RecoveryAction action;
        std::chrono::steady_clock::time_point lastAttempt;
        size_t attemptCount;
        bool inProgress;
    };
    
    std::unordered_map<ErrorType, RecoveryState> recoveryActions_;
    std::chrono::steady_clock::time_point lastErrorTime_;
    std::atomic<size_t> recoveryAttempts_{0};
    std::atomic<bool> autoRecoveryEnabled_{true};
    
    // Cooldown period between recovery attempts
    static constexpr auto RECOVERY_COOLDOWN = std::chrono::seconds(5);
    
    // Maximum attempts before giving up
    static constexpr size_t MAX_RECOVERY_ATTEMPTS = 3;
};

} // namespace AES67

#endif // ERROR_RECOVERY_MANAGER_H