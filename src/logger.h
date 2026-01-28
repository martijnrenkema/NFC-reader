#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

// Log levels
enum class LogLevel {
    INFO,
    WARN,
    ERROR
};

#define MAX_LOG_ENTRIES 100
#define LOG_MESSAGE_SIZE 80

// Single log entry
struct LogEntry {
    time_t epochTime;          // Unix timestamp (0 if NTP not synced)
    unsigned long uptimeMs;    // millis() at log time
    LogLevel level;
    char message[LOG_MESSAGE_SIZE];
};

// Log file path
#define LOG_FILE_PATH "/logs.bin"

class Logger {
public:
    void begin();

    // Log methods
    void info(const char* message);
    void warn(const char* message);
    void error(const char* message);

    // Printf-style logging
    void infof(const char* format, ...);
    void warnf(const char* format, ...);
    void errorf(const char* format, ...);

    // Get logs
    uint16_t getCount();
    const LogEntry* getEntry(uint16_t index);

    // Clear all logs
    void clear();

    // Get JSON representation of all logs
    String toJson();

    // Save logs to SPIFFS (called periodically)
    void save();

    // Check if urgent save is needed
    bool needsUrgentSave() const;

private:
    LogEntry _entries[MAX_LOG_ENTRIES];
    uint16_t _head = 0;
    uint16_t _count = 0;
    bool _dirty = false;
    bool _urgentSave = false;
    unsigned long _lastSave = 0;

    void addEntry(LogLevel level, const char* message);
    const char* levelToString(LogLevel level);
    void loadFromFile();
    void saveToFile();
};

// Global instance
extern Logger logger;

#endif // LOGGER_H
