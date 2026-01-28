#include "storage.h"
#include "config.h"

#include <Preferences.h>
#include <WiFi.h>
static Preferences prefs;
static Preferences tagPrefs;

Storage storage;

void Storage::begin() {
    prefs.begin(NVS_NAMESPACE, false);
    Serial.println("[STORAGE] NVS initialized");

    // Load settings on init
    _settings = load();
    _loaded = true;
}

NFCSettings Storage::load() {
    NFCSettings settings;
    memset(&settings, 0, sizeof(settings));

    // ESP32: Use Preferences
    String ssid = prefs.getString(NVS_WIFI_SSID, "");
    String pass = prefs.getString(NVS_WIFI_PASS, "");
    strlcpy(settings.wifiSsid, ssid.c_str(), sizeof(settings.wifiSsid));
    strlcpy(settings.wifiPassword, pass.c_str(), sizeof(settings.wifiPassword));

    String mqttHost = prefs.getString(NVS_MQTT_HOST, "");
    settings.mqttPort = prefs.getUShort(NVS_MQTT_PORT, 1883);
    String mqttUser = prefs.getString(NVS_MQTT_USER, "");
    String mqttPass = prefs.getString(NVS_MQTT_PASS, "");
    strlcpy(settings.mqttHost, mqttHost.c_str(), sizeof(settings.mqttHost));
    strlcpy(settings.mqttUser, mqttUser.c_str(), sizeof(settings.mqttUser));
    strlcpy(settings.mqttPassword, mqttPass.c_str(), sizeof(settings.mqttPassword));

    String deviceName = prefs.getString(NVS_DEVICE_NAME, "NFC Reader");
    strlcpy(settings.deviceName, deviceName.c_str(), sizeof(settings.deviceName));

    // OTA/AP passwords
    String otaPass = prefs.getString(NVS_OTA_PASSWORD, "");
    String apPass = prefs.getString(NVS_AP_PASSWORD, "");
    strlcpy(settings.otaPassword, otaPass.c_str(), sizeof(settings.otaPassword));
    strlcpy(settings.apPassword, apPass.c_str(), sizeof(settings.apPassword));

    ensureDefaults(settings);
    Serial.println("[STORAGE] Settings loaded");
    return settings;
}

void Storage::save(const NFCSettings& settings) {
    _settings = settings;

    prefs.putString(NVS_WIFI_SSID, settings.wifiSsid);
    prefs.putString(NVS_WIFI_PASS, settings.wifiPassword);
    prefs.putString(NVS_MQTT_HOST, settings.mqttHost);
    prefs.putUShort(NVS_MQTT_PORT, settings.mqttPort);
    prefs.putString(NVS_MQTT_USER, settings.mqttUser);
    prefs.putString(NVS_MQTT_PASS, settings.mqttPassword);
    prefs.putString(NVS_DEVICE_NAME, settings.deviceName);
    // OTA/AP passwords
    prefs.putString(NVS_OTA_PASSWORD, settings.otaPassword);
    prefs.putString(NVS_AP_PASSWORD, settings.apPassword);

    Serial.println("[STORAGE] Settings saved");
}

void Storage::commit() {
    save(_settings);
}

void Storage::setWiFi(const char* ssid, const char* password) {
    strlcpy(_settings.wifiSsid, ssid, sizeof(_settings.wifiSsid));
    strlcpy(_settings.wifiPassword, password, sizeof(_settings.wifiPassword));
    commit();
    Serial.println("[STORAGE] WiFi credentials saved");
}

void Storage::setMQTT(const char* host, uint16_t port, const char* user, const char* password) {
    strlcpy(_settings.mqttHost, host, sizeof(_settings.mqttHost));
    _settings.mqttPort = port;
    strlcpy(_settings.mqttUser, user, sizeof(_settings.mqttUser));
    strlcpy(_settings.mqttPassword, password, sizeof(_settings.mqttPassword));
    commit();
    Serial.println("[STORAGE] MQTT config saved");
}

void Storage::setDeviceName(const char* name) {
    strlcpy(_settings.deviceName, name, sizeof(_settings.deviceName));
    commit();
    Serial.println("[STORAGE] Device name saved");
}

void Storage::setOTAPassword(const char* password) {
    strlcpy(_settings.otaPassword, password, sizeof(_settings.otaPassword));
    commit();
    Serial.println("[STORAGE] OTA password saved");
}

void Storage::setAPPassword(const char* password) {
    strlcpy(_settings.apPassword, password, sizeof(_settings.apPassword));
    commit();
    Serial.println("[STORAGE] AP password saved");
}

const char* Storage::getOTAPassword() {
    if (strlen(_settings.otaPassword) > 0) {
        return _settings.otaPassword;
    }
    return OTA_PASSWORD;
}

const char* Storage::getAPPassword() {
    // WiFi.softAP requires minimum 8 character password
    if (strlen(_settings.apPassword) >= 8) {
        return _settings.apPassword;
    }
    return WIFI_AP_PASSWORD;
}

bool Storage::hasWiFiCredentials() {
    return strlen(_settings.wifiSsid) > 0;
}

bool Storage::hasMQTTConfig() {
    return strlen(_settings.mqttHost) > 0;
}

void Storage::reset() {
    memset(&_settings, 0, sizeof(_settings));
    prefs.clear();
    Serial.println("[STORAGE] Factory reset complete");
}

void Storage::ensureDefaults(NFCSettings& settings) {
    if (settings.mqttPort == 0) settings.mqttPort = 1883;
    if (strlen(settings.deviceName) == 0) {
        strcpy(settings.deviceName, "NFC Reader");
    }
}

// =============================================
// Tag Registry Implementation
// =============================================

bool Storage::registerTag(const char* uid, const char* name) {
    if (!uid || !name || strlen(uid) == 0 || strlen(name) == 0) {
        return false;
    }

    // Sanitize name for HA (replace spaces with underscores, remove special chars)
    char safeName[32];
    size_t j = 0;
    for (size_t i = 0; i < strlen(name) && j < sizeof(safeName) - 1; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            safeName[j++] = c;
        } else if (c == ' ') {
            safeName[j++] = '_';
        }
    }
    safeName[j] = '\0';

    if (strlen(safeName) == 0) {
        return false;
    }

    tagPrefs.begin(NVS_TAG_NAMESPACE, false);

    // Check if already registered (update name)
    uint8_t count = tagPrefs.getUChar(NVS_TAG_COUNT, 0);

    for (uint8_t i = 0; i < count; i++) {
        String keyUid = "uid_" + String(i);
        String storedUid = tagPrefs.getString(keyUid.c_str(), "");
        if (storedUid == uid) {
            // Update existing entry
            String keyName = "name_" + String(i);
            tagPrefs.putString(keyName.c_str(), safeName);
            tagPrefs.end();
            Serial.printf("[STORAGE] Tag updated: %s -> %s\n", uid, safeName);
            return true;
        }
    }

    // Check limit
    if (count >= MAX_REGISTERED_TAGS) {
        tagPrefs.end();
        Serial.println("[STORAGE] Tag registry full");
        return false;
    }

    // Add new entry
    String keyUid = "uid_" + String(count);
    String keyName = "name_" + String(count);
    tagPrefs.putString(keyUid.c_str(), uid);
    tagPrefs.putString(keyName.c_str(), safeName);
    tagPrefs.putUChar(NVS_TAG_COUNT, count + 1);

    tagPrefs.end();
    Serial.printf("[STORAGE] Tag registered: %s -> %s\n", uid, safeName);
    return true;
}

bool Storage::unregisterTag(const char* uid) {
    if (!uid || strlen(uid) == 0) {
        return false;
    }

    tagPrefs.begin(NVS_TAG_NAMESPACE, false);

    uint8_t count = tagPrefs.getUChar(NVS_TAG_COUNT, 0);
    bool found = false;

    for (uint8_t i = 0; i < count; i++) {
        String keyUid = "uid_" + String(i);
        String storedUid = tagPrefs.getString(keyUid.c_str(), "");

        if (!found && storedUid == uid) {
            found = true;
        }

        // Shift remaining entries
        if (found && i < count - 1) {
            String nextKeyUid = "uid_" + String(i + 1);
            String nextKeyName = "name_" + String(i + 1);
            String nextUid = tagPrefs.getString(nextKeyUid.c_str(), "");
            String nextName = tagPrefs.getString(nextKeyName.c_str(), "");

            String currKeyName = "name_" + String(i);
            tagPrefs.putString(keyUid.c_str(), nextUid);
            tagPrefs.putString(currKeyName.c_str(), nextName);
        }
    }

    if (found) {
        // Remove last entry and decrement count
        String lastKeyUid = "uid_" + String(count - 1);
        String lastKeyName = "name_" + String(count - 1);
        tagPrefs.remove(lastKeyUid.c_str());
        tagPrefs.remove(lastKeyName.c_str());
        tagPrefs.putUChar(NVS_TAG_COUNT, count - 1);
        Serial.printf("[STORAGE] Tag unregistered: %s\n", uid);
    }

    tagPrefs.end();
    return found;
}

const char* Storage::getTagName(const char* uid) {
    static char nameBuffer[32];
    nameBuffer[0] = '\0';

    if (!uid || strlen(uid) == 0) {
        return nullptr;
    }

    tagPrefs.begin(NVS_TAG_NAMESPACE, true);  // Read-only

    uint8_t count = tagPrefs.getUChar(NVS_TAG_COUNT, 0);
    for (uint8_t i = 0; i < count; i++) {
        String keyUid = "uid_" + String(i);
        String storedUid = tagPrefs.getString(keyUid.c_str(), "");
        if (storedUid == uid) {
            String keyName = "name_" + String(i);
            String name = tagPrefs.getString(keyName.c_str(), "");
            strlcpy(nameBuffer, name.c_str(), sizeof(nameBuffer));
            tagPrefs.end();
            return nameBuffer;
        }
    }

    tagPrefs.end();
    return nullptr;
}

uint8_t Storage::getRegisteredTagCount() {
    tagPrefs.begin(NVS_TAG_NAMESPACE, true);
    uint8_t count = tagPrefs.getUChar(NVS_TAG_COUNT, 0);
    tagPrefs.end();
    return count;
}

bool Storage::getRegisteredTag(uint8_t index, TagEntry& entry) {
    tagPrefs.begin(NVS_TAG_NAMESPACE, true);

    uint8_t count = tagPrefs.getUChar(NVS_TAG_COUNT, 0);
    if (index >= count) {
        tagPrefs.end();
        return false;
    }

    String keyUid = "uid_" + String(index);
    String keyName = "name_" + String(index);
    String uid = tagPrefs.getString(keyUid.c_str(), "");
    String name = tagPrefs.getString(keyName.c_str(), "");

    strlcpy(entry.uid, uid.c_str(), sizeof(entry.uid));
    strlcpy(entry.name, name.c_str(), sizeof(entry.name));

    tagPrefs.end();
    return true;
}

void Storage::clearTagRegistry() {
    tagPrefs.begin(NVS_TAG_NAMESPACE, false);
    tagPrefs.clear();
    tagPrefs.end();
    Serial.println("[STORAGE] Tag registry cleared");
}
