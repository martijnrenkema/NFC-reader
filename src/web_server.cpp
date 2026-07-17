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

#define FILESYSTEM LittleFS
#define UPDATE_ERROR_STRING() Update.errorString()

// External flag from main.cpp (volatile: set from the async webserver task,
// read by the main loop)
extern volatile bool otaInProgress;

WebServer webServer;

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
        ESP.restart();
    } else if (_pendingRestart) {
        _pendingRestart = false;
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
                request->send(200, "application/json", "{\"success\":true,\"message\":\"Device name saved\"}");
            } else {
                request->send(400, "application/json", "{\"error\":\"Name must be 1-31 characters\"}");
            }
        } else {
            request->send(400, "application/json", "{\"error\":\"Missing name parameter\"}");
        }
    });

    // OTA Update - Firmware
    _server->on("/api/update/firmware", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            bool success = !Update.hasError();
            AsyncWebServerResponse* response = request->beginResponse(
                success ? 200 : 500,
                "text/plain",
                success ? "OK" : "Update failed"
            );
            response->addHeader("Connection", "close");
            request->send(response);
            if (success) {
                _pendingRestart = true;
                _pendingActionTime = millis();
            }
        },
        [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            if (!index) {
                Serial.printf("[OTA] Firmware update start: %s\n", filename.c_str());
                if (Update.isRunning()) {
                    Update.abort();  // clean up a previously aborted upload
                }
                otaInProgress = true;
                // Flag only - the main loop suspends MQTT and updates the
                // LED; PubSubClient/RMT must not be touched from this task
                mqttHandler.requestSuspend();

                // The client can vanish mid-upload; without this the device
                // stays stuck in OTA state (MQTT off, LED blinking) forever
                request->onDisconnect([]() {
                    if (Update.isRunning()) {
                        Update.abort();
                        Serial.println("[OTA] Firmware upload aborted (client disconnected)");
                        otaInProgress = false;
                        mqttHandler.resume();
                    }
                });

                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    Serial.printf("[OTA] Update.begin failed: %s\n", UPDATE_ERROR_STRING());
                    Update.printError(Serial);
                    otaInProgress = false;
                    mqttHandler.resume();
                    return;
                }
                Serial.println("[OTA] Update.begin success");
            }

            if (Update.hasError()) return;

            if (len) {
                if (Update.write(data, len) != len) {
                    Serial.printf("[OTA] Update.write failed: %s\n", UPDATE_ERROR_STRING());
                    otaInProgress = false;
                    mqttHandler.resume();
                    return;
                }
            }

            if (final) {
                if (Update.end(true)) {
                    Serial.printf("[OTA] Firmware update success: %u bytes\n", index + len);
                } else {
                    Serial.printf("[OTA] Firmware update failed: %s\n", UPDATE_ERROR_STRING());
                    Update.printError(Serial);
                    otaInProgress = false;
                    mqttHandler.resume();
                }
            }
        }
    );

    // OTA Update - Filesystem
    _server->on("/api/update/filesystem", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            bool success = !Update.hasError();
            AsyncWebServerResponse* response = request->beginResponse(
                success ? 200 : 500,
                "text/plain",
                success ? "OK" : "Update failed"
            );
            response->addHeader("Connection", "close");
            request->send(response);
            if (success) {
                _pendingRestart = true;
                _pendingActionTime = millis();
            }
        },
        [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            if (!index) {
                Serial.printf("[OTA] Filesystem update start: %s\n", filename.c_str());
                if (Update.isRunning()) {
                    Update.abort();  // clean up a previously aborted upload
                }
                otaInProgress = true;
                mqttHandler.requestSuspend();

                request->onDisconnect([]() {
                    if (Update.isRunning()) {
                        Update.abort();
                        Serial.println("[OTA] Filesystem upload aborted (client disconnected)");
                        otaInProgress = false;
                        mqttHandler.resume();
                    }
                });

                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
                    Serial.printf("[OTA] Update.begin failed: %s\n", UPDATE_ERROR_STRING());
                    Update.printError(Serial);
                    otaInProgress = false;
                    mqttHandler.resume();
                    return;
                }
                Serial.println("[OTA] Update.begin success");
            }

            if (Update.hasError()) return;

            if (len) {
                if (Update.write(data, len) != len) {
                    Serial.printf("[OTA] Update.write failed: %s\n", UPDATE_ERROR_STRING());
                    otaInProgress = false;
                    mqttHandler.resume();
                    return;
                }
            }

            if (final) {
                if (Update.end(true)) {
                    Serial.printf("[OTA] Filesystem update success: %u bytes\n", index + len);
                } else {
                    Serial.printf("[OTA] Filesystem update failed: %s\n", UPDATE_ERROR_STRING());
                    Update.printError(Serial);
                    otaInProgress = false;
                    mqttHandler.resume();
                }
            }
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

        if (uid.length() == 0 || name.length() == 0) {
            request->send(400, "application/json", "{\"error\":\"UID and name cannot be empty\"}");
            return;
        }

        if (storage.registerTag(uid.c_str(), name.c_str())) {
            // Republish MQTT discovery to include the new tag trigger.
            // Flag only - the actual publish happens in the main loop
            // (PubSubClient is not thread-safe).
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

        if (storage.unregisterTag(uid.c_str())) {
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

    // Captive portal redirect
    _server->onNotFound([](AsyncWebServerRequest* request) {
        if (request->method() == HTTP_GET && wifiManager.isAPMode()) {
            String url = request->url();
            if (url == "/" || url == "/index.html") {
                request->send(200, "text/html",
                    "<html><body style='font-family:sans-serif;text-align:center;padding:50px;'>"
                    "<h1>NFC Reader</h1>"
                    "<p>Web interface files missing!</p>"
                    "<p>Please flash the filesystem binary to the device.</p>"
                    "</body></html>");
            } else {
                request->redirect("http://192.168.4.1/");
            }
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
    bool changed = false;

    if (request->hasParam("ota_password", true)) {
        String otaPass = request->getParam("ota_password", true)->value();
        if (otaPass.length() >= 8) {
            storage.setOTAPassword(otaPass.c_str());
            changed = true;
        } else if (otaPass.length() > 0) {
            request->send(400, "application/json", "{\"error\":\"OTA password must be at least 8 characters\"}");
            return;
        }
    }

    if (request->hasParam("ap_password", true)) {
        String apPass = request->getParam("ap_password", true)->value();
        if (apPass.length() >= 8) {
            storage.setAPPassword(apPass.c_str());
            changed = true;
        } else if (apPass.length() > 0) {
            request->send(400, "application/json", "{\"error\":\"AP password must be at least 8 characters\"}");
            return;
        }
    }

    if (changed) {
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Passwords saved. Restart device to apply.\"}");
    } else {
        request->send(400, "application/json", "{\"error\":\"No valid passwords provided\"}");
    }
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
