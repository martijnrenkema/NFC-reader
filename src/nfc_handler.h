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
    const ScanEntry* getHistoryEntry(uint8_t index);  // 0 = newest
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

    // Scan history ring buffer
    ScanEntry _history[NFC_SCAN_HISTORY_SIZE];
    uint8_t _historyHead = 0;
    uint8_t _historyCount = 0;

    // Debounce - prevent same tag being read twice quickly
    char _debounceUID[32] = {0};
    unsigned long _debounceTime = 0;

    void formatUID(uint8_t* uid, uint8_t uidLength, char* output, size_t outputSize);
    void addToHistory(const char* uid);
    bool isDebounced(const char* uid);
};

extern NFCHandler nfcHandler;

#endif // NFC_HANDLER_H
