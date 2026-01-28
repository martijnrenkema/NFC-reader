#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include "config.h"

// Maximum number of tags in registry
#define MAX_REGISTERED_TAGS 50

// Tag entry for registry
struct TagEntry {
    char uid[24];       // UID string (e.g., "C3:7B:70:19")
    char name[32];      // User-friendly name (e.g., "Bedroom Light")
};

// Settings structure - stored in NVS
struct NFCSettings {
    // WiFi
    char wifiSsid[64];
    char wifiPassword[64];

    // MQTT
    char mqttHost[64];
    uint16_t mqttPort;
    char mqttUser[32];
    char mqttPassword[64];

    // Device
    char deviceName[32];

    // Security - configurable passwords
    char otaPassword[32];
    char apPassword[32];
};

class Storage {
public:
    void begin();

    // Load all settings (from NVS - use sparingly)
    NFCSettings load();

    // Get cached settings (fast, no NVS read)
    const NFCSettings& getSettings() { return _settings; }

    // Save all settings
    void save(const NFCSettings& settings);

    // Individual setters
    void setWiFi(const char* ssid, const char* password);
    void setMQTT(const char* host, uint16_t port, const char* user, const char* password);
    void setDeviceName(const char* name);
    void setOTAPassword(const char* password);
    void setAPPassword(const char* password);

    // Password getters
    const char* getOTAPassword();
    const char* getAPPassword();

    // Check if WiFi is configured
    bool hasWiFiCredentials();

    // Check if MQTT is configured
    bool hasMQTTConfig();

    // Factory reset
    void reset();

    // Tag registry
    bool registerTag(const char* uid, const char* name);
    bool unregisterTag(const char* uid);
    const char* getTagName(const char* uid);
    uint8_t getRegisteredTagCount();
    bool getRegisteredTag(uint8_t index, TagEntry& entry);
    void clearTagRegistry();

private:
    NFCSettings _settings;
    bool _loaded = false;

    void ensureDefaults(NFCSettings& settings);
    void commit();
};

extern Storage storage;

#endif // STORAGE_H
