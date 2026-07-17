#ifndef NFC_HANDLER_H
#define NFC_HANDLER_H

#include <Arduino.h>
#include "config.h"

// Scan history entry
struct ScanEntry {
    char uid[32];               // UID formatted as XX:XX:XX:XX...
    unsigned long timestamp;     // millis() when scanned
};

class NFCHandler {
public:
    // Initialize NFC reader
    bool begin();

    // Main loop - check for tags
    void loop();

    // Run diagnostics - prints detailed PN532 status
    void runDiagnostics();

    // Status
    bool isConnected();
    bool isTagPresent();

    // Last scanned tag info
    const char* getLastUID();
    unsigned long getLastScanTime();
    unsigned long timeSinceLastScan();

    // Scan history
    uint8_t getHistoryCount();
    // Copy history (newest first) into out; returns number of entries copied.
    // Snapshot-based so the async webserver task never reads the ring buffer
    // while the main loop is writing it.
    uint8_t copyHistory(ScanEntry* out, uint8_t maxEntries);
    void clearHistory();

    // Callback for new tag scanned
    typedef void (*ScanCallback)(const char* uid);
    void onTagScanned(ScanCallback callback);

private:
    bool _connected = false;
    bool _tagPresent = false;
    char _lastUID[32] = {0};
    unsigned long _lastScanTime = 0;
    unsigned long _lastCheckTime = 0;
    ScanCallback _callback = nullptr;

    // Scan history ring buffer (guarded: written by main loop, read/cleared
    // by the async webserver task)
    ScanEntry _history[NFC_SCAN_HISTORY_SIZE];
    uint8_t _historyHead = 0;
    uint8_t _historyCount = 0;
    portMUX_TYPE _historyMux = portMUX_INITIALIZER_UNLOCKED;

    // Debounce - prevent same tag being read twice quickly
    char _debounceUID[32] = {0};
    unsigned long _debounceTime = 0;

    void formatUID(uint8_t* uid, uint8_t uidLength, char* output, size_t outputSize);
    void addToHistory(const char* uid);
    bool isDebounced(const char* uid);
};

extern NFCHandler nfcHandler;

#endif // NFC_HANDLER_H
