#include <Arduino.h>
#include <time.h>
#include "config.h"

#include "storage.h"
#include "wifi_manager.h"
#include "mqtt_handler.h"
#include "web_server.h"
#include "nfc_handler.h"
#include "led_controller.h"
#include "ota_handler.h"
#include "logger.h"

// Global settings
NFCSettings settings;

// Time sync
bool timeConfigured = false;

// OTA state tracking
bool otaInProgress = false;

// Configure NTP time sync
void setupTimeSync() {
    // Configure time for Europe/Amsterdam timezone (CET/CEST)
    configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
    Serial.println("[TIME] NTP sync configured (CET/CEST with auto DST)");
    timeConfigured = true;
}

// Central LED status update
void updateLedStatus() {
    // 1. OTA has highest priority
    if (otaInProgress) {
        ledController.blink(50);  // Very fast blink during OTA
        return;
    }

    // 2. AP mode
    if (wifiManager.isAPMode()) {
        ledController.showAPMode();
        return;
    }

    // 3. WiFi connecting
    if (wifiManager.getState() == WifiStatus::CONNECTING) {
        ledController.showConnecting();
        return;
    }

    // 4. WiFi disconnected
    if (!wifiManager.isConnected() && !wifiManager.isAPMode()) {
        ledController.showError();
        return;
    }

    // 5. NFC reader not connected
    if (!nfcHandler.isConnected()) {
        ledController.blink(1000);  // Slow blink = NFC error
        return;
    }

    // 6. Tag present - solid on
    if (nfcHandler.isTagPresent()) {
        ledController.on();
        return;
    }

    // 7. Idle - pulsing
    ledController.pulse();
}

// WiFi state change handler
void onWiFiStateChange(WifiStatus state) {
    if (state == WifiStatus::CONNECTED) {
        // Start OTA when connected
        otaHandler.begin();
        // Setup NTP time sync
        setupTimeSync();
    }
    updateLedStatus();
}

// NFC tag scanned handler
void onTagScanned(const char* uid) {
    // Flash LED
    ledController.showScan();

    // Publish to MQTT
    mqttHandler.publishTagScanned(uid);

    // Update LED status
    updateLedStatus();
}

// OTA handlers
void onOTAStart() {
    otaInProgress = true;
    updateLedStatus();
    logger.info("OTA update started");
}

void onOTAEnd() {
    otaInProgress = false;
    updateLedStatus();
    logger.info("OTA update completed");
}

void setup() {
    // Initialize serial
    Serial.begin(SERIAL_BAUD);
    delay(1000);

    Serial.println();
    Serial.println("=================================");
    Serial.println("  NFC Reader for Home Assistant");
    Serial.print("  Firmware v");
    Serial.println(FIRMWARE_VERSION);
    Serial.println("=================================");
    Serial.println();

    // Initialize logger first
    logger.begin();
    logger.infof("System startup - v%s", FIRMWARE_VERSION);

    // Initialize components
    storage.begin();
    settings = storage.getSettings();

    // Initialize LED first for visual feedback
    ledController.begin();
    ledController.showError();  // Red during startup

    // Initialize WiFi
    wifiManager.begin();
    wifiManager.onStateChange(onWiFiStateChange);

    // Check if we have WiFi credentials
    if (storage.hasWiFiCredentials()) {
        Serial.printf("[MAIN] Connecting to saved WiFi: %s\n", settings.wifiSsid);
        wifiManager.connect(settings.wifiSsid, settings.wifiPassword);
    } else {
        Serial.println("[MAIN] No WiFi credentials, starting AP mode");
        wifiManager.startAP();
    }

    // Initialize MQTT
    mqttHandler.begin();
    if (storage.hasMQTTConfig()) {
        Serial.printf("[MAIN] MQTT configured: %s:%d\n", settings.mqttHost, settings.mqttPort);
        mqttHandler.connect(settings.mqttHost, settings.mqttPort,
                           settings.mqttUser, settings.mqttPassword);
    }

    // Initialize NFC reader
    if (nfcHandler.begin()) {
        Serial.println("[MAIN] NFC reader initialized");
        nfcHandler.onTagScanned(onTagScanned);
    } else {
        Serial.println("[MAIN] NFC reader NOT detected - check wiring!");
        logger.error("NFC reader not detected");
    }

    // Initialize web server
    webServer.begin();

    // Setup OTA callbacks
    otaHandler.onStart(onOTAStart);
    otaHandler.onEnd(onOTAEnd);

    // Update LED status
    updateLedStatus();

    Serial.println("[MAIN] Setup complete");
    Serial.println();
}

void loop() {
    // Run all component loops
    wifiManager.loop();
    yield();

    nfcHandler.loop();
    ledController.loop();

    // Update LED status periodically (every 2 seconds - less frequent)
    static unsigned long lastLedUpdate = 0;
    if (millis() - lastLedUpdate >= 2000) {
        lastLedUpdate = millis();
        updateLedStatus();
    }

    otaHandler.loop();
    webServer.loop();
    yield();

    mqttHandler.loop();
    yield();

    // Check for urgent log saves
    if (logger.needsUrgentSave()) {
        logger.save();
    }

    // Periodic tasks every minute
    static unsigned long lastPeriodicTask = 0;
    unsigned long now = millis();
    if (now - lastPeriodicTask >= 60000) {
        lastPeriodicTask = now;

        // Save logs periodically
        logger.save();

        // Publish MQTT state
        mqttHandler.publishState();
    }

    // Give async tasks CPU time
    yield();
}
