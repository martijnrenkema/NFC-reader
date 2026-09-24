#include "web_server.h"
#include "config.h"
#include "storage.h"
#include "wifi_manager.h"
#include "mqtt_handler.h"
#include "nfc_handler.h"
#include "led_controller.h"
#include "logger.h"
#include "update_checker.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_timer.h>
#include "diagnostics.h"

#define FILESYSTEM LittleFS
#define UPDATE_ERROR_STRING() Update.errorString()

// External flag from main.cpp (volatile: set from the async webserver task,
// read by the main loop)
extern volatile bool otaInProgress;

WebServer webServer;

// Served when the LittleFS web files are missing or the filesystem is corrupt
static const char RECOVERY_PAGE[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>NFC Reader - Recovery</title>
<style>body{font:15px/1.5 system-ui,sans-serif;max-width:480px;margin:40px auto;padding:0 16px;color:#09090B;background:#FAFAFA}
h1{font-size:22px}input,button{font:inherit;margin:8px 0;display:block}button{padding:10px 18px;border:0;border-radius:8px;background:#2563EB;color:#fff}
#s{color:#71717A}</style></head><body>
<h1>NFC Reader</h1>
<p>The web interface files are missing. Upload <b>littlefs.bin</b> (or spiffs.bin) from the latest
<a href="https://github.com/)html" UPDATE_GITHUB_REPO R"html(/releases">GitHub release</a> to restore it.</p>
<input type="file" id="f" accept=".bin"><button id="b">Upload web interface</button><p id="s"></p>
<script>
document.getElementById('b').onclick=function(){var f=document.getElementById('f').files[0],s=document.getElementById('s');
if(!f){s.textContent='Choose a file first';return}var d=new FormData();d.append('file',f);var x=new XMLHttpRequest();
x.open('POST','/api/update/filesystem');x.upload.onprogress=function(e){s.textContent='Uploading '+Math.round(e.loaded/e.total*100)+'%'};
x.onload=function(){s.textContent=x.status==200?'Done, restarting...':'Failed: '+x.responseText;if(x.status==200)setTimeout(function(){location.reload()},8000)};
x.onerror=function(){s.textContent='Connection lost'};x.send(d)};
</script></body></html>)html";

// Result of a manual upload, stored per request in _tempObject (freed by the
// request destructor) so concurrent uploads can't clobber each other's result
enum UploadResult : uint8_t { UPLOAD_PENDING = 0, UPLOAD_OK, UPLOAD_FAILED, UPLOAD_REJECTED };

static void setUploadResult(AsyncWebServerRequest* request, UploadResult result) {
    if (!request->_tempObject) {
        request->_tempObject = malloc(sizeof(uint8_t));
    }
    if (request->_tempObject) {
        *(uint8_t*)request->_tempObject = result;
    }
}

// UIDs are formatted by nfc_handler as colon separated hex bytes
// ("5C:9E:35:4A"); reject anything else before it ends up in the registry,
// MQTT topics or the web UI
static bool isValidUid(const String& uid) {
    if (uid.length() < 2 || uid.length() > 23) return false;
    for (size_t i = 0; i < uid.length(); i++) {
        char c = uid[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        if (!hex && c != ':') return false;
    }
    return true;
}

void WebServer::begin() {
    if (_server != nullptr) {
        return;
    }

    // Initialize filesystem
    if (!FILESYSTEM.begin(true)) {
        Serial.println("[WEB] Filesystem mount failed");
    }

    _server = new AsyncWebServer(WEBSERVER_PORT);
    if (_server == nullptr) {
        Serial.println("[WEB] ERROR: Failed to allocate AsyncWebServer!");
        return;
    }

    setupRoutes();
    _server->begin();

    Serial.println("[WEB] Server started on port 80");
}

void WebServer::stop() {
    if (_server != nullptr) {
        _server->end();
        delete _server;
        _server = nullptr;
    }
}

void WebServer::loop() {
    // Deferred MQTT trigger cleanup (handler runs in the async task;
    // PubSubClient may only be used from the main task)
    if (_pendingTriggerCleanup) {
        char uid[sizeof(_pendingCleanupUid)];
        portENTER_CRITICAL(&_pendingMux);
        strlcpy(uid, _pendingCleanupUid, sizeof(uid));
        portEXIT_CRITICAL(&_pendingMux);
        _pendingTriggerCleanup = false;
        if (mqttHandler.isConnected()) {
            mqttHandler.removeOldTagTriggers(uid);
        }
    }

    if (_pendingActionTime == 0) return;

    // Wait for HTTP response to be sent
    if (millis() - _pendingActionTime < 500) return;

    if (_pendingWifiConnect) {
        char ssid[sizeof(_pendingWifiSsid)];
        char pass[sizeof(_pendingWifiPassword)];
        portENTER_CRITICAL(&_pendingMux);
        strlcpy(ssid, _pendingWifiSsid, sizeof(ssid));
        strlcpy(pass, _pendingWifiPassword, sizeof(pass));
        portEXIT_CRITICAL(&_pendingMux);
        _pendingWifiConnect = false;
        wifiManager.connect(ssid, pass);
        if (_settingsCallback) _settingsCallback();
    } else if (_pendingMqttConnect) {
        char host[sizeof(_pendingMqttHost)];
        char user[sizeof(_pendingMqttUser)];
        char pass[sizeof(_pendingMqttPassword)];
        uint16_t port;
        portENTER_CRITICAL(&_pendingMux);
        strlcpy(host, _pendingMqttHost, sizeof(host));
        strlcpy(user, _pendingMqttUser, sizeof(user));
        strlcpy(pass, _pendingMqttPassword, sizeof(pass));
        port = _pendingMqttPort;
        portEXIT_CRITICAL(&_pendingMux);
        _pendingMqttConnect = false;
        mqttHandler.disconnect();
        mqttHandler.connect(host, port, user, pass);
        if (_settingsCallback) _settingsCallback();
    } else if (_pendingReset) {
        _pendingReset = false;
        storage.reset();
        logger.info("Factory reset");
        logger.flush();
        ESP.restart();
    } else if (_pendingRestart) {
        _pendingRestart = false;
        logger.info("Restart requested");
        logger.flush();  // No-op while a filesystem update suspended writes
        ESP.restart();
    }

    // One action handled per pass; keep the timer armed while others are
    // still pending. (A single shared timestamp used to strand a second
    // action forever when two requests arrived within the 500ms window.)
    if (_pendingWifiConnect || _pendingMqttConnect || _pendingReset || _pendingRestart) {
        _pendingActionTime = millis();
    } else {
        _pendingActionTime = 0;
    }
}

void WebServer::onSettingsChanged(SettingsCallback callback) {
    _settingsCallback = callback;
}

void WebServer::setupRoutes() {
    // Serve static files from filesystem
    _server->serveStatic("/", FILESYSTEM, "/").setDefaultFile("index.html");

    // API endpoints
    _server->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleStatus(request);
    });

    _server->on("/api/wifi", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleSaveWifi(request);
    });

    _server->on("/api/mqtt", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleSaveMqtt(request);
    });

    _server->on("/api/reset", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleReset(request);
    });

    _server->on("/api/restart", HTTP_POST, [this](AsyncWebServerRequest* request) {
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Restarting...\"}");
        _pendingRestart = true;
        _pendingActionTime = millis();
    });

    _server->on("/api/passwords", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleSavePasswords(request);
    });

    _server->on("/api/passwords", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleGetPasswords(request);
    });

    // NFC scan history
    _server->on("/api/scans", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleScanHistory(request);
    });

    // Auto-update endpoints
    _server->on("/api/update/check", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleUpdateCheck(request);
    });

    _server->on("/api/update/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleUpdateStatus(request);
    });

    _server->on("/api/update/install", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleUpdateInstall(request);
    });

    _server->on("/api/scans", HTTP_DELETE, [](AsyncWebServerRequest* request) {
        nfcHandler.clearHistory();
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Scan history cleared\"}");
    });

    // System logs
    _server->on("/api/logs", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", logger.toJson());
    });

    _server->on("/api/logs", HTTP_DELETE, [](AsyncWebServerRequest* request) {
        logger.clear();
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Logs cleared\"}");
    });

    // Night mode (LED off for bedroom use)
    _server->on("/api/night_mode", HTTP_GET, [](AsyncWebServerRequest* request) {
        char json[32];
        snprintf(json, sizeof(json), "{\"night_mode\":%s}", ledController.isNightMode() ? "true" : "false");
        request->send(200, "application/json", json);
    });

    _server->on("/api/night_mode", HTTP_POST, [](AsyncWebServerRequest* request) {
        bool enable = false;
        if (request->hasParam("enabled", true)) {
            String val = request->getParam("enabled", true)->value();
            enable = (val == "true" || val == "1" || val == "on");
        }
        ledController.setNightMode(enable);

        // Also publish to MQTT if connected
        if (mqttHandler.isConnected()) {
            mqttHandler.requestStatePublish();
        }

        request->send(200, "application/json", "{\"success\":true,\"night_mode\":" + String(enable ? "true" : "false") + "}");
    });

    // Device settings
    _server->on("/api/device", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (request->hasParam("name", true)) {
            String name = request->getParam("name", true)->value();
            if (name.length() > 0 && name.length() < 32) {
                storage.setDeviceName(name.c_str());
                mqttHandler.requestDiscoveryPublish();  // HA picks up the new name
                request->send(200, "application/json", "{\"success\":true,\"message\":\"Device name saved\"}");
            } else {
                request->send(400, "application/json", "{\"error\":\"Name must be 1-31 characters\"}");
            }
        } else {
            request->send(400, "application/json", "{\"error\":\"Missing name parameter\"}");
        }
    });

    // OTA Update - Firmware / Filesystem (manual upload from the web UI)
    _server->on("/api/update/firmware", HTTP_POST,
        [this](AsyncWebServerRequest* request) { finishUpload(request); },
        [this](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            handleUploadChunk(request, filename, index, data, len, final, U_FLASH);
        }
    );

    _server->on("/api/update/filesystem", HTTP_POST,
        [this](AsyncWebServerRequest* request) { finishUpload(request); },
        [this](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            handleUploadChunk(request, filename, index, data, len, final, U_SPIFFS);
        }
    );

    // Tag Registry - List all registered tags
    _server->on("/api/tags", HTTP_GET, [](AsyncWebServerRequest* request) {
        // 50 tags x (uid + name string copies + object overhead) exceeds 2KB;
        // too small a pool makes ArduinoJson drop entries silently
        DynamicJsonDocument doc(6144);
        JsonArray tags = doc.createNestedArray("tags");

        uint8_t count = storage.getRegisteredTagCount();
        for (uint8_t i = 0; i < count; i++) {
            TagEntry entry;
            if (storage.getRegisteredTag(i, entry)) {
                JsonObject tag = tags.createNestedObject();
                tag["uid"] = entry.uid;
                tag["name"] = entry.name;
            }
        }
        doc["count"] = count;
        doc["max"] = MAX_REGISTERED_TAGS;

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // Tag Registry - Register a tag
    _server->on("/api/tags", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (!request->hasParam("uid", true) || !request->hasParam("name", true)) {
            request->send(400, "application/json", "{\"error\":\"Missing uid or name parameter\"}");
            return;
        }

        String uid = request->getParam("uid", true)->value();
        String name = request->getParam("name", true)->value();
        uid.toUpperCase();

        if (!isValidUid(uid)) {
            request->send(400, "application/json", "{\"error\":\"Invalid UID (expected hex bytes like 5C:9E:35:4A)\"}");
            return;
        }
        if (name.length() == 0 || name.length() > 31) {
            request->send(400, "application/json", "{\"error\":\"Name must be 1-31 characters\"}");
            return;
        }

        // Renaming an existing tag: its old trigger must go
        char oldName[32] = {0};
        bool existed = storage.getTagName(uid.c_str(), oldName, sizeof(oldName));

        if (storage.registerTag(uid.c_str(), name.c_str())) {
            char newName[32] = {0};
            storage.getTagName(uid.c_str(), newName, sizeof(newName));
            if (existed && strcmp(oldName, newName) != 0) {
                mqttHandler.requestTriggerRemoval(oldName);
            }
            // Flags only - discovery (named triggers + event types) is
            // published from the main loop (PubSubClient is not thread-safe)
            mqttHandler.requestDiscoveryPublish();
            request->send(200, "application/json", "{\"success\":true,\"message\":\"Tag registered\"}");
        } else {
            request->send(500, "application/json", "{\"error\":\"Failed to register tag (registry full or invalid name)\"}");
        }
    });

    // Tag Registry - Delete a tag
    _server->on("/api/tags", HTTP_DELETE, [](AsyncWebServerRequest* request) {
        if (!request->hasParam("uid")) {
            request->send(400, "application/json", "{\"error\":\"Missing uid parameter\"}");
            return;
        }

        String uid = request->getParam("uid")->value();
        char name[32] = {0};
        storage.getTagName(uid.c_str(), name, sizeof(name));

        if (storage.unregisterTag(uid.c_str())) {
            // Remove the retained HA trigger, otherwise it lingers forever
            if (name[0]) mqttHandler.requestTriggerRemoval(name);
            mqttHandler.requestDiscoveryPublish();
            request->send(200, "application/json", "{\"success\":true,\"message\":\"Tag unregistered\"}");
        } else {
            request->send(404, "application/json", "{\"error\":\"Tag not found\"}");
        }
    });

    // Remove old UID-based triggers from Home Assistant (v1.3.0 cleanup)
    _server->on("/api/cleanup_trigger", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!request->hasParam("uid", true)) {
            request->send(400, "application/json", "{\"error\":\"Missing uid parameter\"}");
            return;
        }

        String uid = request->getParam("uid", true)->value();
        uid.toUpperCase();
        if (!isValidUid(uid)) {
            request->send(400, "application/json", "{\"error\":\"Invalid UID\"}");
            return;
        }

        if (mqttHandler.isConnected()) {
            // Deferred to loop(): PubSubClient may only be used from the
            // main task
            portENTER_CRITICAL(&_pendingMux);
            strlcpy(_pendingCleanupUid, uid.c_str(), sizeof(_pendingCleanupUid));
            portEXIT_CRITICAL(&_pendingMux);
            _pendingTriggerCleanup = true;
            request->send(200, "application/json", "{\"success\":true,\"message\":\"Trigger removed from Home Assistant\"}");
        } else {
            request->send(503, "application/json", "{\"error\":\"MQTT not connected\"}");
        }
    });

    // Captive portal detection endpoints
    _server->on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(204);
    });
    _server->on("/gen_204", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(204);
    });
    _server->on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/html", "<html><body>Success</body></html>");
    });
    _server->on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/plain", "Microsoft Connect Test");
    });

    // Missing web files (e.g. after a failed filesystem update) or captive
    // portal redirect
    _server->onNotFound([](AsyncWebServerRequest* request) {
        if (request->method() != HTTP_GET) {
            request->send(404);
            return;
        }
        String url = request->url();
        if (url == "/" || url == "/index.html") {
            // serveStatic found no index.html(.gz): serve a built-in page so
            // the web interface can be restored without USB
            request->send_P(200, "text/html", RECOVERY_PAGE);
        } else if (wifiManager.isAPMode()) {
            request->redirect("http://192.168.4.1/");
        } else {
            request->send(404);
        }
    });
}

void WebServer::handleStatus(AsyncWebServerRequest* request) {
    const NFCSettings& settings = storage.getSettings();

    DynamicJsonDocument doc(1024);

    // WiFi status
    doc["wifi"]["connected"] = wifiManager.isConnected();
    doc["wifi"]["ap_mode"] = wifiManager.isAPMode();
    doc["wifi"]["ssid"] = wifiManager.getSSID();
    doc["wifi"]["ip"] = wifiManager.getIP();
    doc["wifi"]["rssi"] = wifiManager.getRSSI();

    // MQTT status
    doc["mqtt"]["connected"] = mqttHandler.isConnected();
    doc["mqtt"]["host"] = settings.mqttHost;
    doc["mqtt"]["port"] = settings.mqttPort;

    // NFC status
    doc["nfc"]["connected"] = nfcHandler.isConnected();
    doc["nfc"]["tag_present"] = nfcHandler.isTagPresent();
    doc["nfc"]["last_uid"] = nfcHandler.getLastUID();
    doc["nfc"]["time_since_scan"] = nfcHandler.timeSinceLastScan() / 1000;  // seconds
    doc["nfc"]["scan_count"] = nfcHandler.getHistoryCount();

    // Device info
    doc["device"]["name"] = settings.deviceName;
    doc["device"]["mac"] = wifiManager.getMacAddress();
    doc["device"]["version"] = FIRMWARE_VERSION;
    doc["device"]["platform"] = "ESP32-C3";
    doc["device"]["night_mode"] = ledController.isNightMode();

    // Diagnostics
    doc["system"]["uptime"] = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    doc["system"]["free_heap"] = ESP.getFreeHeap();
    doc["system"]["min_free_heap"] = ESP.getMinFreeHeap();
    doc["system"]["reset_reason"] = resetReasonString();
    doc["system"]["nfc_reconnects"] = nfcHandler.getReconnectCount();

    String response;
    size_t jsonSize = serializeJson(doc, response);
    if (jsonSize == 0) {
        request->send(500, "application/json", "{\"error\":\"JSON serialization failed\"}");
        return;
    }

    request->send(200, "application/json", response);
}

void WebServer::handleSaveWifi(AsyncWebServerRequest* request) {
    if (!request->hasParam("ssid", true) || !request->hasParam("password", true)) {
        request->send(400, "application/json", "{\"error\":\"Missing parameters\"}");
        return;
    }

    String ssid = request->getParam("ssid", true)->value();
    String password = request->getParam("password", true)->value();

    // Reject instead of silently truncating: a cut-off password can never
    // connect and the device falls back to AP mode
    if (ssid.length() == 0 || ssid.length() > 32) {
        request->send(400, "application/json", "{\"error\":\"SSID must be 1-32 characters\"}");
        return;
    }
    if (password.length() > 0 && (password.length() < 8 || password.length() > 63)) {
        request->send(400, "application/json", "{\"error\":\"WiFi password must be 8-63 characters (or empty for an open network)\"}");
        return;
    }

    storage.setWiFi(ssid.c_str(), password.c_str());

    request->send(200, "application/json", "{\"success\":true,\"message\":\"WiFi saved, connecting...\"}");

    portENTER_CRITICAL(&_pendingMux);
    strlcpy(_pendingWifiSsid, ssid.c_str(), sizeof(_pendingWifiSsid));
    strlcpy(_pendingWifiPassword, password.c_str(), sizeof(_pendingWifiPassword));
    portEXIT_CRITICAL(&_pendingMux);
    _pendingWifiConnect = true;
    _pendingActionTime = millis();
}

void WebServer::handleSaveMqtt(AsyncWebServerRequest* request) {
    if (!request->hasParam("host", true)) {
        request->send(400, "application/json", "{\"error\":\"Missing host parameter\"}");
        return;
    }

    String host = request->getParam("host", true)->value();
    uint16_t port = 1883;
    String user = "";
    String password = "";

    if (request->hasParam("port", true)) {
        int portVal = request->getParam("port", true)->value().toInt();
        if (portVal > 0 && portVal <= 65535) {
            port = portVal;
        }
    }
    if (request->hasParam("user", true)) {
        user = request->getParam("user", true)->value();
    }
    if (request->hasParam("password", true)) {
        password = request->getParam("password", true)->value();
    }

    if (host.length() == 0 || host.length() > 63) {
        request->send(400, "application/json", "{\"error\":\"Broker host must be 1-63 characters\"}");
        return;
    }
    if (user.length() > 31) {
        request->send(400, "application/json", "{\"error\":\"MQTT username can be at most 31 characters\"}");
        return;
    }
    if (password.length() > 63) {
        request->send(400, "application/json", "{\"error\":\"MQTT password can be at most 63 characters\"}");
        return;
    }

    storage.setMQTT(host.c_str(), port, user.c_str(), password.c_str());

    request->send(200, "application/json", "{\"success\":true,\"message\":\"MQTT saved, connecting...\"}");

    portENTER_CRITICAL(&_pendingMux);
    strlcpy(_pendingMqttHost, host.c_str(), sizeof(_pendingMqttHost));
    _pendingMqttPort = port;
    strlcpy(_pendingMqttUser, user.c_str(), sizeof(_pendingMqttUser));
    strlcpy(_pendingMqttPassword, password.c_str(), sizeof(_pendingMqttPassword));
    portEXIT_CRITICAL(&_pendingMux);
    _pendingMqttConnect = true;
    _pendingActionTime = millis();
}

void WebServer::handleReset(AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Resetting...\"}");

    _pendingReset = true;
    _pendingActionTime = millis();
}

void WebServer::handleSavePasswords(AsyncWebServerRequest* request) {
    String otaPass = request->hasParam("ota_password", true) ? request->getParam("ota_password", true)->value() : "";
    String apPass = request->hasParam("ap_password", true) ? request->getParam("ap_password", true)->value() : "";

    // Validate both before saving either. Longer passwords used to be cut
    // off silently at 31 characters, locking users out of the fallback AP.
    if (otaPass.length() > 0 && (otaPass.length() < 8 || otaPass.length() > 63)) {
        request->send(400, "application/json", "{\"error\":\"OTA password must be 8-63 characters\"}");
        return;
    }
    if (apPass.length() > 0 && (apPass.length() < 8 || apPass.length() > 63)) {
        request->send(400, "application/json", "{\"error\":\"Access point password must be 8-63 characters\"}");
        return;
    }
    if (otaPass.length() == 0 && apPass.length() == 0) {
        request->send(400, "application/json", "{\"error\":\"No valid passwords provided\"}");
        return;
    }

    if (otaPass.length() > 0) storage.setOTAPassword(otaPass.c_str());
    if (apPass.length() > 0) storage.setAPPassword(apPass.c_str());
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Passwords saved. Restart device to apply.\"}");
}

void WebServer::handleGetPasswords(AsyncWebServerRequest* request) {
    StaticJsonDocument<128> doc;
    doc["ota_custom"] = strlen(storage.getSettings().otaPassword) > 0;
    doc["ap_custom"] = strlen(storage.getSettings().apPassword) > 0;

    String response;
    if (serializeJson(doc, response) == 0) {
        request->send(500, "application/json", "{\"error\":\"JSON serialization failed\"}");
        return;
    }
    request->send(200, "application/json", response);
}

void WebServer::handleScanHistory(AsyncWebServerRequest* request) {
    DynamicJsonDocument doc(1024);
    JsonArray scans = doc.createNestedArray("scans");

    // Snapshot: this handler runs in the async task while the main loop may
    // be writing new scans into the ring buffer
    ScanEntry history[NFC_SCAN_HISTORY_SIZE];
    uint8_t count = nfcHandler.copyHistory(history, NFC_SCAN_HISTORY_SIZE);
    for (uint8_t i = 0; i < count; i++) {
        JsonObject scan = scans.createNestedObject();
        scan["uid"] = history[i].uid;
        scan["time"] = history[i].timestamp;
        scan["ago"] = (millis() - history[i].timestamp) / 1000;  // seconds ago
    }

    String response;
    if (serializeJson(doc, response) == 0) {
        request->send(500, "application/json", "{\"error\":\"JSON serialization failed\"}");
        return;
    }
    request->send(200, "application/json", response);
}

void WebServer::handleUpdateCheck(AsyncWebServerRequest* request) {
    // Trigger manual update check
    updateChecker.checkForUpdates();
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Update check started\"}");
}

void WebServer::handleUpdateStatus(AsyncWebServerRequest* request) {
    UpdateInfo info;
    updateChecker.getInfoSnapshot(info);
    UpdateCheckState state = updateChecker.getState();

    StaticJsonDocument<512> doc;
    doc["state"] = state == UpdateCheckState::IDLE ? "idle" :
                   state == UpdateCheckState::CHECKING ? "checking" :
                   state == UpdateCheckState::DOWNLOADING ? "downloading" : "error";
    doc["available"] = info.available;
    doc["current_version"] = info.currentVersion;
    doc["latest_version"] = info.latestVersion;
    doc["release_url"] = info.releaseUrl;
    doc["download_progress"] = info.downloadProgress;
    doc["error"] = info.errorMessage;

    // Time since last check (in seconds)
    if (info.lastCheckTime > 0) {
        doc["last_check_ago"] = (millis() - info.lastCheckTime) / 1000;
    } else {
        doc["last_check_ago"] = -1;  // Never checked
    }

    // Has download URLs
    doc["has_firmware_url"] = strlen(info.downloadUrl) > 0;
    doc["has_spiffs_url"] = strlen(info.spiffsUrl) > 0;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void WebServer::handleUpdateInstall(AsyncWebServerRequest* request) {
    if (!updateChecker.isUpdateAvailable()) {
        request->send(400, "application/json", "{\"error\":\"No update available\"}");
        return;
    }

    UpdateCheckState state = updateChecker.getState();
    if (state == UpdateCheckState::CHECKING || state == UpdateCheckState::DOWNLOADING) {
        request->send(400, "application/json", "{\"error\":\"Update already in progress\"}");
        return;
    }

    // Start OTA update
    updateChecker.startOTAUpdate();
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Update started\"}");
}

void WebServer::handleUploadChunk(AsyncWebServerRequest* request, const String& filename, size_t index,
                                  uint8_t* data, size_t len, bool final, int command) {
    const char* label = (command == U_FLASH) ? "Firmware" : "Filesystem";

    if (!index) {
        // One update at a time: a GitHub install, ArduinoOTA or another
        // upload may already be writing flash. (Aborting it here used to
        // corrupt the running update.)
        if (otaInProgress || Update.isRunning()) {
            Serial.printf("[OTA] %s upload rejected: another update is running\n", label);
            setUploadResult(request, UPLOAD_REJECTED);
            return;
        }
        setUploadResult(request, UPLOAD_PENDING);
        Serial.printf("[OTA] %s update start: %s\n", label, filename.c_str());
        logger.infof("%s upload started", label);

        otaInProgress = true;
        // Flag only - the main loop suspends MQTT and updates the LED;
        // PubSubClient/RMT must not be touched from this task
        mqttHandler.requestSuspend();

        if (command == U_SPIFFS) {
            // The image overwrites the mounted LittleFS partition: stop log
            // writes (waits for a running save) and unmount first, otherwise
            // a concurrent write corrupts the new image
            logger.flush();
            logger.suspendFileWrites();
            FILESYSTEM.end();
        }

        // The client can vanish mid-upload; without this the device stays
        // stuck in OTA state (MQTT off, LED blinking) forever
        request->onDisconnect([this, command]() {
            if (Update.isRunning()) {
                Update.abort();
                Serial.println("[OTA] Upload aborted (client disconnected)");
                uploadFailed(command);
            }
        });

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, command)) {
            Serial.printf("[OTA] Update.begin failed: %s\n", UPDATE_ERROR_STRING());
            setUploadResult(request, UPLOAD_FAILED);
            uploadFailed(command);
            return;
        }
    }

    // Rejected or already failed: ignore the rest of the body
    if (!request->_tempObject || *(uint8_t*)request->_tempObject != UPLOAD_PENDING) return;
    if (!Update.isRunning()) return;

    if (len && Update.write(data, len) != len) {
        Serial.printf("[OTA] Update.write failed: %s\n", UPDATE_ERROR_STRING());
        Update.abort();
        setUploadResult(request, UPLOAD_FAILED);
        uploadFailed(command);
        return;
    }

    if (final) {
        if (Update.end(true)) {
            Serial.printf("[OTA] %s update success: %u bytes\n", label, index + len);
            setUploadResult(request, UPLOAD_OK);
        } else {
            Serial.printf("[OTA] %s update failed: %s\n", label, UPDATE_ERROR_STRING());
            setUploadResult(request, UPLOAD_FAILED);
            uploadFailed(command);
        }
    }
}

void WebServer::uploadFailed(int command) {
    otaInProgress = false;
    mqttHandler.resume();
    if (command == U_SPIFFS) {
        // Remount (no format): if the partition was partly overwritten the
        // mount fails and the recovery page takes over
        FILESYSTEM.begin(false);
        logger.resumeFileWrites();
    }
    logger.warn("Manual update failed");
}

void WebServer::finishUpload(AsyncWebServerRequest* request) {
    // No file part at all (e.g. an empty POST) counts as failed, so it can't
    // trigger a restart
    uint8_t result = request->_tempObject ? *(uint8_t*)request->_tempObject : (uint8_t)UPLOAD_FAILED;

    int code = 500;
    const char* msg = "Update failed";
    if (result == UPLOAD_OK) {
        code = 200;
        msg = "OK";
    } else if (result == UPLOAD_REJECTED) {
        code = 409;
        msg = "Another update is in progress";
    }

    AsyncWebServerResponse* response = request->beginResponse(code, "text/plain", msg);
    response->addHeader("Connection", "close");
    request->send(response);

    if (result == UPLOAD_OK) {
        _pendingRestart = true;
        _pendingActionTime = millis();
    }
}
