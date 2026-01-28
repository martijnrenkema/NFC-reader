#include "storage.h"
#include "config.h"

#include <Preferences.h>
#include <WiFi.h>
static Preferences prefs;

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
