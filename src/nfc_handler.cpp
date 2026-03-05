#include "nfc_handler.h"
#include "config.h"
#include "logger.h"
#include <Wire.h>
#include <PN532_I2C.h>
#include <PN532.h>

// PN532 I2C instance using Seeed-Studio library
static PN532_I2C* pn532i2c = nullptr;
static PN532* nfc = nullptr;

NFCHandler nfcHandler;

bool NFCHandler::begin() {
    // Hardware reset PN532 using RSTO pin
    Serial.println("[NFC] Hardware reset via RSTO pin...");
    pinMode(PN532_RESET_PIN, OUTPUT);
    digitalWrite(PN532_RESET_PIN, HIGH);
    delay(100);
    digitalWrite(PN532_RESET_PIN, LOW);   // Reset pulse
    delay(100);
    digitalWrite(PN532_RESET_PIN, HIGH);  // Release reset
    delay(500);  // Wait for PN532 to boot

    // Initialize I2C with custom pins for ESP32-C3
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    // CRITICAL: Configure I2C timeout for PN532
    // PN532 uses clock stretching up to 1ms (datasheet p.211)
    // Default ESP32 timeout is only 100µs which causes hangs
    Wire.setTimeout(10);  // 10ms timeout for I2C operations
#if defined(ESP32)
    Wire.setTimeOut(10);  // ESP32-specific timeout (in ms)
#endif

    delay(100);

    // Create PN532 objects
    if (pn532i2c == nullptr) {
        pn532i2c = new PN532_I2C(Wire);
        nfc = new PN532(*pn532i2c);
    }

    // Initialize PN532
    nfc->begin();

    // Check if PN532 is present
    uint32_t versiondata = nfc->getFirmwareVersion();
    if (!versiondata) {
        Serial.println("[NFC] PN532 not found - check wiring!");
        logger.error("PN532 not found");
        _connected = false;
        return false;
    }

    // Print chip info
    Serial.printf("[NFC] Found PN5%02X, Firmware: %d.%d\n",
                  (versiondata >> 24) & 0xFF,
                  (versiondata >> 16) & 0xFF,
                  (versiondata >> 8) & 0xFF);

    // Configure board to read RFID tags
    Serial.println("[NFC] Configuring SAM...");
    bool samOK = nfc->SAMConfig();
    Serial.printf("[NFC] SAMConfig: %s\n", samOK ? "OK" : "FAILED");

    if (!samOK) {
        Serial.println("[NFC] SAMConfig failed - trying again...");
        delay(100);
        samOK = nfc->SAMConfig();
        Serial.printf("[NFC] SAMConfig retry: %s\n", samOK ? "OK" : "FAILED");
    }

    _connected = true;
    logger.info("NFC reader initialized");
    Serial.println("[NFC] Reader initialized successfully");

    return true;
}

void NFCHandler::loop() {
    if (!_connected) return;

    unsigned long now = millis();

    // Check for tags at configured interval
    if (now - _lastCheckTime < NFC_CHECK_INTERVAL_MS) return;
    _lastCheckTime = now;

    // Debug: show we're scanning (every 5 seconds)
    static unsigned long lastDebug = 0;
    if (now - lastDebug > 5000) {
        Serial.println("[NFC] Scanning for tags...");
        lastDebug = now;
    }

    // Try to read a tag
    uint8_t uid[10];  // Max UID size (7 for Mifare, 10 for some ISO14443A)
    uint8_t uidLength = 0;

    // readPassiveTargetID with short timeout (30ms)
    bool success = nfc->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 30);

    // Give WiFi stack time to process
    yield();

    if (success && uidLength > 0 && uidLength <= sizeof(uid)) {
        // Format UID as string
        char uidStr[32];
        formatUID(uid, uidLength, uidStr, sizeof(uidStr));

        // Check debounce
        if (!isDebounced(uidStr)) {
            // New tag detected!
            strncpy(_lastUID, uidStr, sizeof(_lastUID) - 1);
            _lastScanTime = now;
            _tagPresent = true;

            // Update debounce
            strncpy(_debounceUID, uidStr, sizeof(_debounceUID) - 1);
            _debounceTime = now;

            // Add to history
            addToHistory(uidStr);

            // Log
            Serial.printf("[NFC] Tag scanned: %s\n", uidStr);
            logger.infof("NFC tag: %s", uidStr);

            // Callback - with yield() to let WiFi stack breathe
            if (_callback) {
                yield();  // Give WiFi/MQTT time before callback
                _callback(uidStr);
                yield();  // Give WiFi/MQTT time after callback
            }
        }
        _tagPresent = true;
    } else {
        _tagPresent = false;
    }
}

bool NFCHandler::isConnected() {
    return _connected;
}

bool NFCHandler::isTagPresent() {
    return _tagPresent;
}

const char* NFCHandler::getLastUID() {
    return _lastUID;
}

unsigned long NFCHandler::getLastScanTime() {
    return _lastScanTime;
}

unsigned long NFCHandler::timeSinceLastScan() {
    if (_lastScanTime == 0) return 0;
    return millis() - _lastScanTime;
}

uint8_t NFCHandler::getHistoryCount() {
    return _historyCount;
}

const ScanEntry* NFCHandler::getHistoryEntry(uint8_t index) {
    if (index >= _historyCount) return nullptr;

    // Calculate position (newest first)
    uint8_t pos = (_historyHead - 1 - index + NFC_SCAN_HISTORY_SIZE) % NFC_SCAN_HISTORY_SIZE;
    return &_history[pos];
}

void NFCHandler::clearHistory() {
    _historyCount = 0;
    _historyHead = 0;
    memset(_history, 0, sizeof(_history));
    Serial.println("[NFC] History cleared");
}

void NFCHandler::onTagScanned(ScanCallback callback) {
    _callback = callback;
}

void NFCHandler::formatUID(uint8_t* uid, uint8_t uidLength, char* output, size_t outputSize) {
    // Format as XX:XX:XX:XX... (colon separated hex)
    // Optimized: O(n) instead of O(n²) by tracking position
    // Required buffer: uidLength * 3 (2 hex + colon) - 1 (no trailing colon) + 1 (null) = uidLength * 3
    if (outputSize < (size_t)(uidLength * 3)) {
        output[0] = '\0';  // Buffer too small
        return;
    }

    char* ptr = output;
    char* end = output + outputSize - 1;  // Leave room for null terminator

    for (uint8_t i = 0; i < uidLength && ptr < end; i++) {
        if (i > 0 && ptr < end) {
            *ptr++ = ':';
        }
        // Write hex directly to buffer with bounds check
        size_t remaining = end - ptr;
        if (remaining >= 2) {
            ptr += snprintf(ptr, remaining + 1, "%02X", uid[i]);
        }
    }
    *ptr = '\0';
}

void NFCHandler::addToHistory(const char* uid) {
    // Add to ring buffer
    ScanEntry& entry = _history[_historyHead];
    strncpy(entry.uid, uid, sizeof(entry.uid) - 1);
    entry.uid[sizeof(entry.uid) - 1] = '\0';
    entry.timestamp = millis();

    _historyHead = (_historyHead + 1) % NFC_SCAN_HISTORY_SIZE;
    if (_historyCount < NFC_SCAN_HISTORY_SIZE) {
        _historyCount++;
    }
}

bool NFCHandler::isDebounced(const char* uid) {
    // Check if this UID was scanned within debounce time
    if (strcmp(uid, _debounceUID) == 0) {
        if (millis() - _debounceTime < NFC_DEBOUNCE_MS) {
            return true;  // Same tag within debounce window
        }
    }
    return false;
}

void NFCHandler::runDiagnostics() {
    // Simplified diagnostics
    Serial.println("\n========== PN532 DIAGNOSTICS ==========");

    if (!nfc) {
        Serial.println("ERROR: PN532 not initialized");
        Serial.println("========================================\n");
        return;
    }

    uint32_t versiondata = nfc->getFirmwareVersion();
    if (versiondata) {
        Serial.printf("Firmware: PN5%02X v%d.%d\n",
                      (versiondata >> 24) & 0xFF,
                      (versiondata >> 16) & 0xFF,
                      (versiondata >> 8) & 0xFF);
    } else {
        Serial.println("ERROR: Cannot read firmware version");
    }

    Serial.println("========================================\n");
}
