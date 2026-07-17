#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <Arduino.h>
#include "config.h"

// Main LED mode
enum class LedMode {
    OFF,
    ON,
    BLINK_FAST,
    BLINK_SLOW,
    PULSE
};

// Non-blocking scan flash state machine
enum class ScanFlashState {
    IDLE,
    FLASH1_ON,
    FLASH1_OFF,
    FLASH2_ON,
    DONE
};

class LedController {
public:
    void begin();
    void loop();

    // Control methods
    void on();
    void off();
    void blink(uint16_t intervalMs);
    void pulse();

    // Mode control
    void setMode(LedMode mode);
    LedMode getMode() { return _mode; }

    // Status display helpers
    void showScan();       // Quick flash on NFC scan (non-blocking!)
    void showAPMode();     // Slow pulse in AP mode (orange)
    void showConnecting(); // Fast blink while connecting (light blue)
    void showConnected();  // Soft pulse when connected (green)
    void showError();      // Fast blink on error (red)

    // Night mode (LED off for bedroom use)
    void setNightMode(bool enabled);
    bool isNightMode() { return _nightMode; }

private:
    // RGB LED helper
    void setRGB(uint8_t r, uint8_t g, uint8_t b);

    LedMode _mode = LedMode::OFF;
    LedMode _previousMode = LedMode::OFF;  // To restore after scan flash
    bool _ledState = false;
    unsigned long _lastToggle = 0;
    uint16_t _blinkInterval = 500;

    // Current color for the current mode
    uint8_t _colorR = 0;
    uint8_t _colorG = 50;
    uint8_t _colorB = 0;

    // Pulse/breathing variables
    uint8_t _pulseValue = 0;
    bool _pulseUp = true;
    unsigned long _lastPulseUpdate = 0;

    // Scan flash state machine (non-blocking)
    ScanFlashState _scanState = ScanFlashState::IDLE;
    unsigned long _scanFlashTime = 0;

    // Night mode (RAM only - no flash writes!)
    // volatile: toggled from the async webserver task, applied in loop()
    volatile bool _nightMode = false;
    volatile bool _nightDirty = false;
};

extern LedController ledController;

#endif // LED_CONTROLLER_H
