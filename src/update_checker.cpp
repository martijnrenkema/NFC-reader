#include "update_checker.h"
#include "config.h"
#include "logger.h"
#include "mqtt_handler.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <stdarg.h>

UpdateChecker updateChecker;

// GitHub API endpoint
static const char* GITHUB_API_URL = "https://api.github.com/repos/" UPDATE_GITHUB_REPO "/releases/latest";
static const char* GITHUB_RELEASES_URL = "https://github.com/" UPDATE_GITHUB_REPO "/releases";

void UpdateChecker::begin() {
    // Store boot time for overflow-safe first check timing
    _bootTime = millis();

    // Initialize current version
    strlcpy(_info.currentVersion, FIRMWARE_VERSION, sizeof(_info.currentVersion));
    memset(_info.latestVersion, 0, sizeof(_info.latestVersion));
    memset(_info.downloadUrl, 0, sizeof(_info.downloadUrl));
    memset(_info.spiffsUrl, 0, sizeof(_info.spiffsUrl));
    memset(_info.releaseUrl, 0, sizeof(_info.releaseUrl));
    memset(_info.errorMessage, 0, sizeof(_info.errorMessage));
    _info.available = false;
    _info.lastCheckTime = 0;
    _info.downloadProgress = 0;

    // Set release URL to default
    strlcpy(_info.releaseUrl, GITHUB_RELEASES_URL, sizeof(_info.releaseUrl));

    logger.info("Update checker initialized");
}

void UpdateChecker::loop() {
    // ERROR counts as "ready for a new attempt": it persists (so the UI can
    // see it) until the next check starts
    bool canStart = (_state == UpdateCheckState::IDLE || _state == UpdateCheckState::ERROR);

    // Handle requested check
    if (_checkRequested && canStart) {
        _checkRequested = false;
        startCheckTask();
        return;
    }

    // Auto-check logic (only when WiFi connected and not busy)
    if (canStart && WiFi.status() == WL_CONNECTED) {
        unsigned long now = millis();
        bool shouldAutoCheck = false;

        // Check 2 minutes after boot, then every 24 hours
        if (!_firstAutoCheckDone) {
            if (now - _bootTime >= 120000) {
                shouldAutoCheck = true;
            }
        } else if (now - _lastAutoCheck >= UPDATE_CHECK_INTERVAL) {
            shouldAutoCheck = true;
        }

        if (shouldAutoCheck) {
            _firstAutoCheckDone = true;
            _lastAutoCheck = now;
            startCheckTask();
            return;
        }
    }

    // Handle OTA update request (stays in the main loop on purpose: the
    // device restarts afterwards and flash writes shouldn't compete with
    // other work)
    if (_otaRequested && canStart) {
        _otaRequested = false;
        performOTAUpdate();
    }
}

void UpdateChecker::checkForUpdates() {
    if (_state == UpdateCheckState::CHECKING || _state == UpdateCheckState::DOWNLOADING) {
        logger.warn("Update check already in progress");
        return;
    }
    _checkRequested = true;
}

void UpdateChecker::startCheckTask() {
    _state = UpdateCheckState::CHECKING;
    if (xTaskCreate(checkTask, "upd_check", 12288, this, 1, nullptr) != pdPASS) {
        logger.warn("Update check task create failed, checking inline");
        performCheck();
    }
}

void UpdateChecker::checkTask(void* arg) {
    static_cast<UpdateChecker*>(arg)->performCheck();
    vTaskDelete(nullptr);
}

void UpdateChecker::performCheck() {
    _state = UpdateCheckState::CHECKING;
    if (_stateCallback) _stateCallback();

    // Work on a local copy; commit under the mutex when done so readers
    // (webserver task, MQTT state publish) never see a half-written struct
    UpdateInfo info;
    getInfoSnapshot(info);
    memset(info.errorMessage, 0, sizeof(info.errorMessage));

    bool ok = false;
    if (WiFi.status() != WL_CONNECTED) {
        strlcpy(info.errorMessage, "WiFi not connected", sizeof(info.errorMessage));
    } else {
        logger.info("Checking for updates...");
        ok = fetchGitHubRelease(info);
    }

    if (ok) {
        info.lastCheckTime = millis();
        if (info.available) {
            logger.infof("Update available: v%s", info.latestVersion);
        } else {
            logger.info("Firmware is up to date");
        }
    } else {
        logger.warnf("Update check failed: %s", info.errorMessage);
    }

    {
        std::lock_guard<std::mutex> lock(_infoMutex);
        _info = info;
    }

    // ERROR persists until the next check so the UI can actually observe it
    _state = ok ? UpdateCheckState::IDLE : UpdateCheckState::ERROR;
    if (_stateCallback) _stateCallback();
}

void UpdateChecker::getInfoSnapshot(UpdateInfo& out) {
    std::lock_guard<std::mutex> lock(_infoMutex);
    out = _info;
}

void UpdateChecker::setErrorf(const char* format, ...) {
    char buffer[sizeof(_info.errorMessage)];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(_infoMutex);
    strlcpy(_info.errorMessage, buffer, sizeof(_info.errorMessage));
}

bool UpdateChecker::fetchGitHubRelease(UpdateInfo& info) {
    WiFiClientSecure client;
    client.setInsecure();  // Skip certificate verification
    client.setTimeout(UPDATE_CHECK_TIMEOUT / 1000);  // ESP32 uses seconds

    HTTPClient http;
    http.setTimeout(UPDATE_CHECK_TIMEOUT);
    http.setUserAgent("ESP-NFC-Reader/" FIRMWARE_VERSION);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, GITHUB_API_URL)) {
        strlcpy(info.errorMessage, "HTTP begin failed", sizeof(info.errorMessage));
        return false;
    }

    // GitHub API requires Accept header
    http.addHeader("Accept", "application/vnd.github.v3+json");

    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        if (httpCode == 403) {
            strlcpy(info.errorMessage, "Rate limited", sizeof(info.errorMessage));
        } else if (httpCode == 404) {
            strlcpy(info.errorMessage, "No releases found", sizeof(info.errorMessage));
        } else if (httpCode < 0) {
            snprintf(info.errorMessage, sizeof(info.errorMessage), "Connection failed: %d", httpCode);
        } else {
            snprintf(info.errorMessage, sizeof(info.errorMessage), "HTTP error: %d", httpCode);
        }
        http.end();
        return false;
    }

    // Get response
    String payload = http.getString();
    http.end();

    if (payload.length() == 0) {
        strlcpy(info.errorMessage, "Empty response", sizeof(info.errorMessage));
        return false;
    }

    return parseReleaseJson(payload.c_str(), payload.length(), info);
}

bool UpdateChecker::parseReleaseJson(const char* json, size_t length, UpdateInfo& info) {
    // Use a filter to only parse the fields we need (reduces memory usage significantly)
    // GitHub API response is ~10KB but we only need a few fields
    StaticJsonDocument<200> filter;
    filter["tag_name"] = true;
    filter["html_url"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;

    // Parse JSON response with filter - this drastically reduces memory needed
    DynamicJsonDocument doc(1536);
    DeserializationError err = deserializeJson(doc, json, length, DeserializationOption::Filter(filter));

    if (err) {
        snprintf(info.errorMessage, sizeof(info.errorMessage), "JSON error: %s", err.c_str());
        return false;
    }

    // Get tag_name (e.g., "v1.7.0")
    const char* tagName = doc["tag_name"];
    if (!tagName || strlen(tagName) == 0) {
        strlcpy(info.errorMessage, "No tag_name in response", sizeof(info.errorMessage));
        return false;
    }

    // Strip 'v' prefix if present
    const char* versionStr = tagName;
    if (versionStr[0] == 'v' || versionStr[0] == 'V') {
        versionStr++;
    }
    strlcpy(info.latestVersion, versionStr, sizeof(info.latestVersion));

    // Store release URL
    const char* htmlUrl = doc["html_url"];
    if (htmlUrl) {
        strlcpy(info.releaseUrl, htmlUrl, sizeof(info.releaseUrl));
    }

    // Compare versions
    info.available = compareVersions(info.latestVersion, info.currentVersion) > 0;

    // Find firmware and filesystem download URLs
    // NFC-reader uses exact filename match: firmware.bin and spiffs.bin
    JsonArray assets = doc["assets"];
    for (JsonObject asset : assets) {
        const char* name = asset["name"];
        if (name) {
            const char* downloadUrl = asset["browser_download_url"];
            if (downloadUrl) {
                // Exact match for firmware.bin
                if (strcmp(name, "firmware.bin") == 0) {
                    strlcpy(info.downloadUrl, downloadUrl, sizeof(info.downloadUrl));
                }
                // Exact match for littlefs.bin (or legacy spiffs.bin)
                else if (strcmp(name, "littlefs.bin") == 0 || strcmp(name, "spiffs.bin") == 0) {
                    strlcpy(info.spiffsUrl, downloadUrl, sizeof(info.spiffsUrl));
                }
            }
        }
    }

    return true;
}

int UpdateChecker::compareVersions(const char* v1, const char* v2) {
    // Parse semantic version: MAJOR.MINOR.PATCH
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;

    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);

    if (major1 != major2) return major1 - major2;
    if (minor1 != minor2) return minor1 - minor2;
    return patch1 - patch2;
}

void UpdateChecker::startOTAUpdate() {
    if (_state == UpdateCheckState::CHECKING || _state == UpdateCheckState::DOWNLOADING) {
        logger.warn("Cannot start OTA: busy");
        return;
    }

    UpdateInfo info;
    getInfoSnapshot(info);
    if (!info.available) {
        setErrorf("No update available");
        return;
    }
    if (strlen(info.downloadUrl) == 0) {
        setErrorf("No download URL");
        return;
    }

    _otaRequested = true;
}

bool UpdateChecker::downloadAndInstall(const char* url, int updateType, const char* label) {
    logger.infof("Downloading %s from: %s", label, url);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(60);

    HTTPClient http;
    http.setTimeout(60000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, url)) {
        setErrorf("%s: HTTP begin failed", label);
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        setErrorf("%s failed: %d", label, httpCode);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        setErrorf("%s: invalid size", label);
        http.end();
        return false;
    }

    logger.infof("%s size: %d bytes", label, contentLength);

    if (!Update.begin(contentLength, updateType)) {
        setErrorf("%s begin failed: %s", label, Update.errorString());
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[1024];
    size_t written = 0;
    size_t lastProgress = 0;
    unsigned long lastDataTime = millis();
    const unsigned long OTA_STREAM_TIMEOUT = 30000;

    while (http.connected() && written < (size_t)contentLength) {
        size_t available = stream->available();
        if (available > 0) {
            size_t toRead = min(available, sizeof(buffer));
            size_t bytesRead = stream->readBytes(buffer, toRead);

            yield();

            if (Update.write(buffer, bytesRead) != bytesRead) {
                setErrorf("%s write failed", label);
                Update.abort();
                http.end();
                return false;
            }

            written += bytesRead;
            lastDataTime = millis();
            _info.downloadProgress = (written * 100) / contentLength;

            if (_info.downloadProgress >= lastProgress + 10) {
                lastProgress = _info.downloadProgress;
                logger.infof("%s progress: %d%%", label, _info.downloadProgress);
                if (_stateCallback) _stateCallback();
            }

            yield();
        } else {
            if (millis() - lastDataTime > OTA_STREAM_TIMEOUT) {
                setErrorf("%s timeout", label);
                Update.abort();
                http.end();
                return false;
            }
            delay(10);
        }
        yield();
    }

    http.end();

    if (written != (size_t)contentLength) {
        setErrorf("%s incomplete", label);
        Update.abort();
        return false;
    }

    if (!Update.end(true)) {
        setErrorf("%s end failed: %s", label, Update.errorString());
        return false;
    }

    logger.infof("%s complete!", label);
    return true;
}

void UpdateChecker::performOTAUpdate() {
    _state = UpdateCheckState::DOWNLOADING;
    {
        std::lock_guard<std::mutex> lock(_infoMutex);
        _info.downloadProgress = 0;
        memset(_info.errorMessage, 0, sizeof(_info.errorMessage));
    }
    if (_stateCallback) _stateCallback();

    // Work from a snapshot of the URLs (the check task could theoretically
    // update _info, though the state machine prevents overlap)
    UpdateInfo info;
    getInfoSnapshot(info);

    // Disconnect MQTT to free resources and prevent timeout errors during long download
    mqttHandler.disconnect();

    // Step 1: Download and install firmware
    if (!downloadAndInstall(info.downloadUrl, U_FLASH, "Firmware")) {
        // ERROR persists so the UI can see what went wrong; MQTT reconnects
        // automatically via its state machine
        _state = UpdateCheckState::ERROR;
        if (_stateCallback) _stateCallback();
        return;
    }

    // Step 2: Download and install filesystem (if URL available)
    if (strlen(info.spiffsUrl) > 0) {
        _info.downloadProgress = 0;
        if (_stateCallback) _stateCallback();

        if (!downloadAndInstall(info.spiffsUrl, U_SPIFFS, "Filesystem")) {
            // Filesystem failed, but firmware was already installed
            // Log warning but still restart to apply firmware
            logger.warn("Filesystem update failed, but firmware was installed");
        }
    }

    logger.info("OTA update complete! Restarting...");
    _info.downloadProgress = 100;
    if (_stateCallback) _stateCallback();

    delay(1000);
    ESP.restart();
}
