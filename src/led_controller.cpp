#include "led_controller.h"
#include "config.h"

LedController ledController;

// RGB LED brightness scale (0-255, but keep low to avoid eye strain)
#define RGB_MAX_BRIGHTNESS 50
#define RGB_SCAN_BRIGHTNESS 80  // Brighter for scan feedback

void LedController::begin() {
    // Setup GPIO10 for external LED (if connected)
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    _mode = LedMode::OFF;
    _ledState = false;
    _scanState = ScanFlashState::IDLE;

    // Turn off RGB LED initially
    setRGB(0, 0, 0);

    Serial.println("[LED] Controller initialized (RGB + GPIO10)");
}

void LedController::setRGB(uint8_t r, uint8_t g, uint8_t b) {
    // Skip if night mode is active (keep LED off)
    if (_nightMode) {
        neopixelWrite(RGB_LED_PIN, 0, 0, 0);
        return;
    }
    // Use built-in neopixelWrite for ESP32-C3 RGB LED
    neopixelWrite(RGB_LED_PIN, r, g, b);
}

void LedController::loop() {
    unsigned long now = millis();

    // Skip all LED updates if night mode is active
    if (_nightMode) {
        // Still process scan flash state machine to track state
        if (_scanState != ScanFlashState::IDLE) {
            unsigned long elapsed = now - _scanFlashTime;
            if (_scanState == ScanFlashState::FLASH1_ON && elapsed >= 80) {
                _scanState = ScanFlashState::FLASH1_OFF;
                _scanFlashTime = now;
            } else if (_scanState == ScanFlashState::FLASH1_OFF && elapsed >= 50) {
                _scanState = ScanFlashState::FLASH2_ON;
                _scanFlashTime = now;
            } else if (_scanState == ScanFlashState::FLASH2_ON && elapsed >= 80) {
                _scanState = ScanFlashState::DONE;
            } else if (_scanState == ScanFlashState::DONE) {
                _scanState = ScanFlashState::IDLE;
                _mode = _previousMode;
            }
        }
        return;  // Skip all visual updates
    }

    // Handle scan flash state machine FIRST (takes priority)
    if (_scanState != ScanFlashState::IDLE) {
        unsigned long elapsed = now - _scanFlashTime;

        switch (_scanState) {
            case ScanFlashState::FLASH1_ON:
                if (elapsed >= 80) {
                    setRGB(0, 0, 0);  // Off
                    digitalWrite(LED_PIN, LOW);
                    _scanState = ScanFlashState::FLASH1_OFF;
                    _scanFlashTime = now;
                }
                break;

            case ScanFlashState::FLASH1_OFF:
                if (elapsed >= 50) {
                    setRGB(0, RGB_SCAN_BRIGHTNESS, RGB_SCAN_BRIGHTNESS);  // Cyan flash
                    digitalWrite(LED_PIN, HIGH);
                    _scanState = ScanFlashState::FLASH2_ON;
                    _scanFlashTime = now;
                }
                break;

            case ScanFlashState::FLASH2_ON:
                if (elapsed >= 80) {
                    setRGB(0, 0, 0);  // Off
                    digitalWrite(LED_PIN, LOW);
                    _scanState = ScanFlashState::DONE;
                    _scanFlashTime = now;
                }
                break;

            case ScanFlashState::DONE:
                // Return to previous mode
                _scanState = ScanFlashState::IDLE;
                _mode = _previousMode;
                _lastToggle = now;  // Reset timing for smooth transition
                break;

            default:
                break;
        }
        return;  // Don't process normal mode while flashing
    }

    // Normal mode handling
    switch (_mode) {
        case LedMode::OFF:
            if (_ledState) {
                setRGB(0, 0, 0);
                digitalWrite(LED_PIN, LOW);
                _ledState = false;
            }
            break;

        case LedMode::ON:
            if (!_ledState) {
                setRGB(_colorR, _colorG, _colorB);
                digitalWrite(LED_PIN, HIGH);
                _ledState = true;
            }
            break;

        case LedMode::BLINK_FAST:
        case LedMode::BLINK_SLOW:
            if (now - _lastToggle >= _blinkInterval) {
                _lastToggle = now;
                _ledState = !_ledState;
                if (_ledState) {
                    setRGB(_colorR, _colorG, _colorB);
                    digitalWrite(LED_PIN, HIGH);
                } else {
                    setRGB(0, 0, 0);
                    digitalWrite(LED_PIN, LOW);
                }
            }
            break;

        case LedMode::PULSE:
            // Breathing effect - update every 20ms
            if (now - _lastPulseUpdate >= 20) {
                _lastPulseUpdate = now;

                if (_pulseUp) {
                    _pulseValue += 2;
                    if (_pulseValue >= RGB_MAX_BRIGHTNESS) {
                        _pulseValue = RGB_MAX_BRIGHTNESS;
                        _pulseUp = false;
                    }
                } else {
                    if (_pulseValue >= 2) {
                        _pulseValue -= 2;
                    } else {
                        _pulseValue = 0;
                        _pulseUp = true;
                    }
                }

                // Scale the color by pulse value
                uint8_t r = (_colorR * _pulseValue) / RGB_MAX_BRIGHTNESS;
                uint8_t g = (_colorG * _pulseValue) / RGB_MAX_BRIGHTNESS;
                uint8_t b = (_colorB * _pulseValue) / RGB_MAX_BRIGHTNESS;
                setRGB(r, g, b);

                // External LED: on when pulse is above half
                digitalWrite(LED_PIN, _pulseValue > (RGB_MAX_BRIGHTNESS / 2) ? HIGH : LOW);
            }
            break;
    }
}

void LedController::on() {
    setMode(LedMode::ON);
}

void LedController::off() {
    setMode(LedMode::OFF);
}

void LedController::blink(uint16_t intervalMs) {
    _blinkInterval = intervalMs;
    setMode(intervalMs <= 200 ? LedMode::BLINK_FAST : LedMode::BLINK_SLOW);
}

void LedController::pulse() {
    setMode(LedMode::PULSE);
}

void LedController::setMode(LedMode mode) {
    if (_mode == mode && _scanState == ScanFlashState::IDLE) return;

    _mode = mode;
    _lastToggle = millis();

    switch (mode) {
        case LedMode::BLINK_FAST:
            _blinkInterval = LED_BLINK_FAST;
            break;
        case LedMode::BLINK_SLOW:
            _blinkInterval = LED_BLINK_SLOW;
            break;
        case LedMode::PULSE:
            _pulseValue = 0;
            _pulseUp = true;
            _lastPulseUpdate = millis();
            break;
        case LedMode::OFF:
            setRGB(0, 0, 0);
            digitalWrite(LED_PIN, LOW);
            _ledState = false;
            break;
        default:
            break;
    }
}

void LedController::showScan() {
    // NON-BLOCKING scan flash using state machine
    // Save current mode to restore after flash
    if (_scanState == ScanFlashState::IDLE) {
        _previousMode = _mode;
    }

    // Start the flash sequence
    _scanState = ScanFlashState::FLASH1_ON;
    _scanFlashTime = millis();

    // Immediately turn on cyan
    setRGB(0, RGB_SCAN_BRIGHTNESS, RGB_SCAN_BRIGHTNESS);
    digitalWrite(LED_PIN, HIGH);
}

void LedController::showAPMode() {
    // Orange/yellow slow pulse for AP mode
    _colorR = RGB_MAX_BRIGHTNESS;
    _colorG = RGB_MAX_BRIGHTNESS / 2;  // Orange
    _colorB = 0;
    setMode(LedMode::PULSE);
}

void LedController::showConnecting() {
    // Light blue fast blink while connecting
    _colorR = 0;
    _colorG = RGB_MAX_BRIGHTNESS / 2;
    _colorB = RGB_MAX_BRIGHTNESS;
    _blinkInterval = LED_BLINK_FAST;
    setMode(LedMode::BLINK_FAST);
}

void LedController::showConnected() {
    // Green soft pulse when connected (idle state)
    _colorR = 0;
    _colorG = RGB_MAX_BRIGHTNESS;
    _colorB = 0;
    setMode(LedMode::PULSE);
}

void LedController::showError() {
    // Red fast blink on error
    _colorR = RGB_MAX_BRIGHTNESS;
    _colorG = 0;
    _colorB = 0;
    _blinkInterval = 100;
    setMode(LedMode::BLINK_FAST);
}

void LedController::setNightMode(bool enabled) {
    _nightMode = enabled;
    if (enabled) {
        // Immediately turn off LED
        setRGB(0, 0, 0);
        digitalWrite(LED_PIN, LOW);
    }
    Serial.printf("[LED] Night mode: %s\n", enabled ? "ON" : "OFF");
}
