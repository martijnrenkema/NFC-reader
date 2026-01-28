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

    // Load tag registry into RAM cache
    loadTagCache();
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
// Tag Registry Implementation (with RAM cache)
// =============================================

void Storage::loadTagCache() {
    tagPrefs.begin(NVS_TAG_NAMESPACE, true);
    _tagCacheCount = tagPrefs.getUChar(NVS_TAG_COUNT, 0);

    for (uint8_t i = 0; i < _tagCacheCount && i < MAX_REGISTERED_TAGS; i++) {
        char keyUid[8], keyName[10];
        snprintf(keyUid, sizeof(keyUid), "uid_%d", i);
        snprintf(keyName, sizeof(keyName), "name_%d", i);

        String uid = tagPrefs.getString(keyUid, "");
        String name = tagPrefs.getString(keyName, "");

        strlcpy(_tagCache[i].uid, uid.c_str(), sizeof(_tagCache[i].uid));
        strlcpy(_tagCache[i].name, name.c_str(), sizeof(_tagCache[i].name));
    }

    tagPrefs.end();
    Serial.printf("[STORAGE] Tag cache loaded: %d tags\n", _tagCacheCount);
}

void Storage::saveTagToNVS(uint8_t index) {
    if (index >= _tagCacheCount) return;

    tagPrefs.begin(NVS_TAG_NAMESPACE, false);

    char keyUid[8], keyName[10];
    snprintf(keyUid, sizeof(keyUid), "uid_%d", index);
    snprintf(keyName, sizeof(keyName), "name_%d", index);

    tagPrefs.putString(keyUid, _tagCache[index].uid);
    tagPrefs.putString(keyName, _tagCache[index].name);

    tagPrefs.end();
}

void Storage::saveTagCountToNVS() {
    tagPrefs.begin(NVS_TAG_NAMESPACE, false);
    tagPrefs.putUChar(NVS_TAG_COUNT, _tagCacheCount);
    tagPrefs.end();
}

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

    // Check if already registered (update name) - RAM lookup
    for (uint8_t i = 0; i < _tagCacheCount; i++) {
        if (strcmp(_tagCache[i].uid, uid) == 0) {
            // Update existing entry in cache and NVS
            strlcpy(_tagCache[i].name, safeName, sizeof(_tagCache[i].name));
            saveTagToNVS(i);
            Serial.printf("[STORAGE] Tag updated: %s -> %s\n", uid, safeName);
            return true;
        }
    }

    // Check limit
    if (_tagCacheCount >= MAX_REGISTERED_TAGS) {
        Serial.println("[STORAGE] Tag registry full");
        return false;
    }

    // Add new entry to cache
    strlcpy(_tagCache[_tagCacheCount].uid, uid, sizeof(_tagCache[_tagCacheCount].uid));
    strlcpy(_tagCache[_tagCacheCount].name, safeName, sizeof(_tagCache[_tagCacheCount].name));
    _tagCacheCount++;

    // Save to NVS
    saveTagToNVS(_tagCacheCount - 1);
    saveTagCountToNVS();

    Serial.printf("[STORAGE] Tag registered: %s -> %s\n", uid, safeName);
    return true;
}

bool Storage::unregisterTag(const char* uid) {
    if (!uid || strlen(uid) == 0) {
        return false;
    }

    // Find in cache
    int foundIndex = -1;
    for (uint8_t i = 0; i < _tagCacheCount; i++) {
        if (strcmp(_tagCache[i].uid, uid) == 0) {
            foundIndex = i;
            break;
        }
    }

    if (foundIndex < 0) {
        return false;
    }

    // Shift remaining entries in cache
    for (uint8_t i = foundIndex; i < _tagCacheCount - 1; i++) {
        memcpy(&_tagCache[i], &_tagCache[i + 1], sizeof(TagEntry));
    }
    _tagCacheCount--;

    // Clear last entry
    memset(&_tagCache[_tagCacheCount], 0, sizeof(TagEntry));

    // Rebuild NVS (simpler than shifting in NVS)
    tagPrefs.begin(NVS_TAG_NAMESPACE, false);
    tagPrefs.clear();
    tagPrefs.putUChar(NVS_TAG_COUNT, _tagCacheCount);

    for (uint8_t i = 0; i < _tagCacheCount; i++) {
        char keyUid[8], keyName[10];
        snprintf(keyUid, sizeof(keyUid), "uid_%d", i);
        snprintf(keyName, sizeof(keyName), "name_%d", i);
        tagPrefs.putString(keyUid, _tagCache[i].uid);
        tagPrefs.putString(keyName, _tagCache[i].name);
    }
    tagPrefs.end();

    Serial.printf("[STORAGE] Tag unregistered: %s\n", uid);
    return true;
}

bool Storage::getTagName(const char* uid, char* buffer, size_t bufferSize) {
    if (!uid || strlen(uid) == 0 || !buffer || bufferSize == 0) {
        return false;
    }

    // Fast RAM lookup - no NVS access!
    for (uint8_t i = 0; i < _tagCacheCount; i++) {
        if (strcmp(_tagCache[i].uid, uid) == 0) {
            strlcpy(buffer, _tagCache[i].name, bufferSize);
            return true;
        }
    }

    return false;
}

uint8_t Storage::getRegisteredTagCount() {
    return _tagCacheCount;  // From cache, no NVS access
}

bool Storage::getRegisteredTag(uint8_t index, TagEntry& entry) {
    if (index >= _tagCacheCount) {
        return false;
    }

    // From cache, no NVS access
    memcpy(&entry, &_tagCache[index], sizeof(TagEntry));
    return true;
}

void Storage::clearTagRegistry() {
    // Clear cache
    _tagCacheCount = 0;
    memset(_tagCache, 0, sizeof(_tagCache));

    // Clear NVS
    tagPrefs.begin(NVS_TAG_NAMESPACE, false);
    tagPrefs.clear();
    tagPrefs.end();

    Serial.println("[STORAGE] Tag registry cleared");
}
