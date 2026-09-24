#include "mqtt_handler.h"
#include "config.h"
#include "nfc_handler.h"
#include "wifi_manager.h"
#include "storage.h"
#include "logger.h"
#include "led_controller.h"
#include "update_checker.h"
#include "diagnostics.h"
#include <esp_timer.h>
#include <ArduinoJson.h>

MQTTHandler mqttHandler;
MQTTHandler* _mqttInstance = nullptr;

void MQTTHandler::begin() {
    _mqttInstance = this;

    // Set socket timeouts for better responsiveness
    // NOTE: WiFiClient::setTimeout takes SECONDS on arduino-esp32 core 2.x
    // (pinned via platformio.ini). This also bounds the blocking TCP connect.
    _wifiClient.setTimeout(2);  // 2 second TCP timeout

    _mqttClient.setClient(_wifiClient);
    _mqttClient.setKeepAlive(NFC_MQTT_KEEPALIVE);
    _mqttClient.setSocketTimeout(5);  // 5 second MQTT socket timeout (handshake needs time)
    // Event entity discovery lists every registered tag name (up to 50 x 31
    // chars), so the buffer must hold ~2 KB plus topic and header
    _mqttClient.setBufferSize(3072);

    // Generate unique device ID from MAC
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char id[13];
    snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    _deviceId = id;

    Serial.println("[MQTT] Handler initialized");
}

void MQTTHandler::processConnectStateMachine() {
    unsigned long now = millis();

    switch (_connectState) {
        case MqttConnectState::IDLE:
            // Check if we should attempt connection
            if (!_mqttClient.connected() && _host.length() > 0 && wifiManager.isConnected()) {
                if (now - _lastReconnect >= _reconnectInterval) {
                    _lastReconnect = now;
                    _connectStartTime = now;

                    // Start TCP connection (non-blocking on ESP32)
                    Serial.printf("[MQTT] Connecting to %s:%d...\n", _host.c_str(), _port);
                    if (_wifiClient.connect(_host.c_str(), _port)) {
                        // TCP connected immediately
                        _connectState = MqttConnectState::MQTT_CONNECTING;
                        Serial.println("[MQTT] TCP connected, starting MQTT handshake...");
                    } else {
                        // TCP connection in progress or failed
                        _connectState = MqttConnectState::TCP_CONNECTING;
                    }
                }
            }
            break;

        case MqttConnectState::TCP_CONNECTING:
            // Check TCP connection status
            if (_wifiClient.connected()) {
                _connectState = MqttConnectState::MQTT_CONNECTING;
                _connectStartTime = now;
                Serial.println("[MQTT] TCP connected, starting MQTT handshake...");
            } else if (now - _connectStartTime >= TCP_CONNECT_TIMEOUT) {
                // TCP timeout
                Serial.println("[MQTT] TCP connection timeout");
                _wifiClient.stop();
                _reconnectInterval = min(_reconnectInterval * 2, 60000UL);
                _connectState = MqttConnectState::IDLE;
            }
            // Yield to allow background processing
            yield();
            break;

        case MqttConnectState::MQTT_CONNECTING: {
            // Attempt MQTT connect (this is still somewhat blocking but with short timeout)
            _mqttClient.setCallback(mqttCallback);
            String clientId = "nfc-reader-" + _deviceId;

            if (_mqttClient.connect(clientId.c_str(), _user.c_str(), _password.c_str(),
                                    (getBaseTopic() + "/availability").c_str(), 0, true, "offline")) {
                Serial.println("[MQTT] Connected");
                logger.infof("MQTT connected to %s:%d", _host.c_str(), _port);

                publishAvailability(true);
                subscribeToCommands();

                // Start discovery state machine
                if (!_discoveryPublished) {
                    _publishState = MqttPublishState::DISCOVERY;
                    _discStep = 0;
                    _lastPublishStep = millis();
                    Serial.println("[MQTT] Starting discovery publish...");
                } else {
                    _publishState = MqttPublishState::STATE_LAST_UID;
                    _lastPublishStep = millis();
                }
                _reconnectInterval = MQTT_RECONNECT_INTERVAL;
                _connectState = MqttConnectState::CONNECTED;
            } else {
                Serial.printf("[MQTT] Connection failed, rc=%d\n", _mqttClient.state());
                logger.errorf("MQTT connection failed (rc=%d)", _mqttClient.state());
                _wifiClient.stop();
                _reconnectInterval = min(_reconnectInterval * 2, 60000UL);
                _connectState = MqttConnectState::IDLE;
            }
            break;
        }

        case MqttConnectState::CONNECTED:
            // Check if still connected
            if (!_mqttClient.connected()) {
                Serial.println("[MQTT] Disconnected");
                logger.warn("MQTT disconnected");
                _connectState = MqttConnectState::IDLE;
            }
            break;

        case MqttConnectState::FAILED:
            // Reset to idle after failure
            _connectState = MqttConnectState::IDLE;
            break;
    }
}

void MQTTHandler::loop() {
    // OTA upload (async task) requests suspension via a flag; the actual
    // disconnect happens here in the main task because PubSubClient is not
    // thread-safe.
    if (_suspendRequested) {
        if (!_suspended) {
            _suspended = true;
            disconnect();
        }
        return;
    }
    if (_suspended) {
        _suspended = false;
        _lastReconnect = millis();  // don't reconnect instantly after resume
    }

    // Restart button pressed in Home Assistant
    if (_restartRequested) {
        logger.info("Restart requested via MQTT");
        logger.flush();
        disconnect();
        delay(100);  // let the offline message leave the socket
        ESP.restart();
    }

    // Process non-blocking connection state machine
    processConnectStateMachine();

    // Refresh the cached flag here (main task) so other tasks can read the
    // connection state via isConnected() without touching PubSubClient
    _connected = _mqttClient.connected();

    if (_connected) {
        _mqttClient.loop();

        // Process non-blocking publish state machine
        processPublishStateMachine();

        if (_publishState == MqttPublishState::IDLE) {
            processTriggerRemovals();
        }

        // Handle discovery publish requests (e.g. tag registered via web UI)
        if (_discoveryRequested && _publishState == MqttPublishState::IDLE) {
            _discoveryRequested = false;
            publishDiscovery();
        }

        // Handle state publish requests
        // Note: volatile bool is sufficient - do NOT use noInterrupts() on ESP32-C3
        if (_statePublishPending && _publishState == MqttPublishState::IDLE) {
            _statePublishPending = false;
            _publishState = MqttPublishState::STATE_LAST_UID;
            _lastPublishStep = millis();
        }

        // Publish state periodically
        unsigned long now = millis();
        if (now - _lastStatePublish >= 30000 && _publishState == MqttPublishState::IDLE) {
            _publishState = MqttPublishState::STATE_LAST_UID;
            _lastPublishStep = millis();
            _lastStatePublish = now;
        }
    }
}

void MQTTHandler::processPublishStateMachine() {
    if (_publishState == MqttPublishState::IDLE) return;
    if (!_mqttClient.connected()) {
        _publishState = MqttPublishState::IDLE;
        return;
    }

    unsigned long now = millis();
    if (now - _lastPublishStep < PUBLISH_STEP_DELAY) return;

    _lastPublishStep = now;
    String base = getBaseTopic();

    switch (_publishState) {
        case MqttPublishState::DISCOVERY:
            if (!publishDiscoveryStep(_discStep++)) {
                _discTagIndex = 0;
                _publishState = MqttPublishState::DISCOVERY_TAGS;
            }
            break;

        case MqttPublishState::DISCOVERY_TAGS: {
            // Named triggers for all registered tags (RAM cache, no NVS), so
            // a new tag shows up in HA right away instead of after its first
            // scan
            TagEntry entry;
            if (storage.getRegisteredTag(_discTagIndex, entry)) {
                publishNamedTagTriggerDiscovery(entry.name);
                _discTagIndex++;
            } else {
                Serial.println("[MQTT] Discovery published");
                _discoveryPublished = true;
                _publishState = MqttPublishState::STATE_LAST_UID;
            }
            break;
        }

        // State publish states
        case MqttPublishState::STATE_LAST_UID: {
            // Nothing scanned since boot: keep the retained value HA already
            // has instead of wiping it with an empty string
            const char* uid = nfcHandler.getLastUID();
            if (uid[0]) {
                _mqttClient.publish((base + "/last_uid").c_str(), uid, true);
            }
            _publishState = MqttPublishState::STATE_TAG_PRESENT;
            break;
        }

        case MqttPublishState::STATE_TAG_PRESENT:
            _mqttClient.publish((base + "/tag_present").c_str(), nfcHandler.isTagPresent() ? "ON" : "OFF", true);
            _publishState = MqttPublishState::STATE_WIFI;
            break;

        case MqttPublishState::STATE_WIFI: {
            char rssiStr[8];
            snprintf(rssiStr, sizeof(rssiStr), "%d", wifiManager.getRSSI());
            _mqttClient.publish((base + "/wifi_signal").c_str(), rssiStr, true);
            _publishState = MqttPublishState::STATE_NIGHT_MODE;
            break;
        }

        case MqttPublishState::STATE_NIGHT_MODE:
            _mqttClient.publish((base + "/night_mode").c_str(), ledController.isNightMode() ? "ON" : "OFF", true);
            _publishState = MqttPublishState::STATE_UPDATE;
            break;

        case MqttPublishState::STATE_UPDATE: {
            // Publish update-related states (snapshot: the check task may be
            // writing the info struct concurrently)
            UpdateInfo info;
            updateChecker.getInfoSnapshot(info);
            _mqttClient.publish((base + "/update_available").c_str(), info.available ? "ON" : "OFF", true);
            _mqttClient.publish((base + "/latest_version").c_str(), info.latestVersion[0] ? info.latestVersion : "unknown", true);
            _mqttClient.publish((base + "/current_version").c_str(), info.currentVersion, true);
            _publishState = MqttPublishState::STATE_DIAGNOSTICS;
            break;
        }

        case MqttPublishState::STATE_DIAGNOSTICS:
            publishDiagnostics();
            _publishState = MqttPublishState::STATE_DONE;
            break;

        case MqttPublishState::STATE_DONE:
            _publishState = MqttPublishState::IDLE;
            break;

        default:
            _publishState = MqttPublishState::IDLE;
            break;
    }

    _mqttClient.loop();
}

void MQTTHandler::connect(const char* host, uint16_t port, const char* user, const char* password) {
    _host = host;
    _port = port;
    _user = user;
    _password = password;

    _mqttClient.setServer(host, port);
    _discoveryPublished = false;
    _lastReconnect = 0;
    _reconnectInterval = MQTT_RECONNECT_INTERVAL;

    Serial.printf("[MQTT] Configured: %s:%d\n", host, port);
}

void MQTTHandler::disconnect() {
    if (_mqttClient.connected()) {
        publishAvailability(false);
        _mqttClient.disconnect();
    }
    _connected = false;
}

void MQTTHandler::publishTagScanned(const char* uid) {
    if (!_mqttClient.connected()) return;

    String base = getBaseTopic();

    // Publish to tag/scanned topic with UID as payload. Feeds the generic
    // device trigger and the HA tag scanner (Tags panel, tag_scanned event).
    _mqttClient.publish((base + "/tag/scanned").c_str(), uid, false);
    _mqttClient.loop();
    yield();

    // Also update retained last_uid (for sensor display)
    _mqttClient.publish((base + "/last_uid").c_str(), uid, true);

    // Update tag present
    _mqttClient.publish((base + "/tag_present").c_str(), "ON", true);
    _mqttClient.loop();
    yield();

    // Check if this is a registered tag - fire named trigger
    // Fast RAM lookup (no NVS access)
    char tagName[32];
    bool known = storage.getTagName(uid, tagName, sizeof(tagName));
    if (known) {
        // Publish to named tag topic for HA device trigger
        String namedTopic = base + "/tag/" + String(tagName);
        _mqttClient.publish(namedTopic.c_str(), uid, false);
        _mqttClient.loop();
        yield();
        Serial.printf("[MQTT] Published named tag: %s (%s)\n", tagName, uid);
    }

    // Event entity: event_type is the tag name ("unknown" for unregistered
    // tags); kept outside tag/ so existing tag/# subscribers are unaffected
    String ev;
    ev.reserve(96);
    ev = "{\"event_type\":\"";
    ev += known ? tagName : "unknown";
    ev += "\",\"uid\":\"";
    ev += uid;
    ev += "\"}";
    _mqttClient.publish((base + "/event").c_str(), ev.c_str(), false);
    _mqttClient.loop();
    yield();

    Serial.printf("[MQTT] Published tag: %s\n", uid);
}

void MQTTHandler::publishDiscovery() {
    if (_publishState == MqttPublishState::IDLE) {
        _publishState = MqttPublishState::DISCOVERY;
        _discStep = 0;
        _lastPublishStep = millis();
        Serial.println("[MQTT] Publishing Home Assistant discovery...");
    }
}

// Device block shared by all entities: name from the settings, link to the
// web UI and the MAC so HA can merge it with the network device
String MQTTHandler::deviceJson() {
    String name = storage.getSettings().deviceName;
    name.replace("\\", "\\\\");
    name.replace("\"", "\\\"");

    String mac;
    for (uint8_t i = 0; i < 12; i += 2) {
        if (i) mac += ':';
        mac += _deviceId.substring(i, i + 2);
    }

    String d;
    d.reserve(256);
    d = "{\"ids\":[\"nfc_reader_" + _deviceId + "\"],";
    d += "\"name\":\"" + name + "\",";
    d += "\"mf\":\"DIY\",\"mdl\":\"ESP32-C3 + PN532\",\"sw\":\"" FIRMWARE_VERSION "\",";
    d += "\"cns\":[[\"mac\",\"" + mac + "\"]]";
    if (wifiManager.isConnected()) {
        d += ",\"cu\":\"http://" + wifiManager.getIP() + "/\"";
    }
    d += "}";
    return d;
}

// payload is an entity config without the closing brace; the device block
// is appended here
void MQTTHandler::publishConfig(const char* component, const char* objectId, String& payload) {
    payload += ",\"dev\":" + deviceJson() + "}";
    String topic = String(MQTT_DISCOVERY_PREFIX) + "/" + component + "/nfcr_" + _deviceId + "_" + objectId + "/config";
    if (!_mqttClient.publish(topic.c_str(), payload.c_str(), true)) {
        Serial.printf("[MQTT] Discovery publish failed: %s (%u bytes)\n", topic.c_str(), payload.length());
    }
}

bool MQTTHandler::publishDiscoveryStep(uint8_t step) {
    String b = getBaseTopic();
    String id = "nfcr_" + _deviceId;
    String avty = ",\"avty_t\":\"" + b + "/availability\"";
    String diag = ",\"ent_cat\":\"diagnostic\"";
    String p;
    p.reserve(640);

    switch (step) {
        case 0:
            p = "{\"name\":\"Last Scanned UID\",\"uniq_id\":\"" + id + "_uid\",\"stat_t\":\"" + b + "/last_uid\"" + avty + ",\"ic\":\"mdi:nfc\"";
            publishConfig("sensor", "uid", p);
            return true;
        case 1:
            p = "{\"name\":\"Tag Present\",\"uniq_id\":\"" + id + "_tag\",\"stat_t\":\"" + b + "/tag_present\"" + avty + ",\"dev_cla\":\"presence\",\"ic\":\"mdi:nfc-variant\"";
            publishConfig("binary_sensor", "tag", p);
            return true;
        case 2:
            p = "{\"name\":\"WiFi Signal\",\"uniq_id\":\"" + id + "_wifi\",\"stat_t\":\"" + b + "/wifi_signal\"" + avty + ",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"stat_cla\":\"measurement\"" + diag;
            publishConfig("sensor", "wifi", p);
            return true;
        case 3:
            p = "{\"name\":\"Night Mode\",\"uniq_id\":\"" + id + "_night\",\"stat_t\":\"" + b + "/night_mode\",\"cmd_t\":\"" + b + "/night_mode/set\"" + avty + ",\"ic\":\"mdi:weather-night\"";
            publishConfig("switch", "night", p);
            return true;
        case 4:
            // Generic device trigger for any tag - UID in trigger.payload
            p = "{\"automation_type\":\"trigger\",\"type\":\"tag_scanned\",\"subtype\":\"nfc\",\"topic\":\"" + b + "/tag/scanned\"";
            publishConfig("device_automation", "scan", p);
            return true;
        case 5:
            p = "{\"name\":\"Update Available\",\"uniq_id\":\"" + id + "_update\",\"stat_t\":\"" + b + "/update_available\"" + avty + ",\"dev_cla\":\"update\",\"ic\":\"mdi:package-up\"" + diag;
            publishConfig("binary_sensor", "update", p);
            return true;
        case 6:
            p = "{\"name\":\"Latest Version\",\"uniq_id\":\"" + id + "_latest_ver\",\"stat_t\":\"" + b + "/latest_version\"" + avty + ",\"ic\":\"mdi:new-box\"" + diag;
            publishConfig("sensor", "latest_ver", p);
            return true;
        case 7:
            p = "{\"name\":\"Current Version\",\"uniq_id\":\"" + id + "_current_ver\",\"stat_t\":\"" + b + "/current_version\"" + avty + ",\"ic\":\"mdi:tag\"" + diag;
            publishConfig("sensor", "current_ver", p);
            return true;
        case 8:
            p = "{\"name\":\"Uptime\",\"uniq_id\":\"" + id + "_uptime\",\"stat_t\":\"" + b + "/diagnostics\",\"val_tpl\":\"{{ value_json.uptime }}\"" + avty + ",\"dev_cla\":\"duration\",\"unit_of_meas\":\"s\",\"stat_cla\":\"total_increasing\"" + diag;
            publishConfig("sensor", "uptime", p);
            return true;
        case 9:
            p = "{\"name\":\"Free Memory\",\"uniq_id\":\"" + id + "_heap\",\"stat_t\":\"" + b + "/diagnostics\",\"val_tpl\":\"{{ value_json.free_heap }}\"" + avty + ",\"dev_cla\":\"data_size\",\"unit_of_meas\":\"B\",\"stat_cla\":\"measurement\",\"ic\":\"mdi:memory\"" + diag;
            publishConfig("sensor", "heap", p);
            return true;
        case 10:
            p = "{\"name\":\"Last Reset Reason\",\"uniq_id\":\"" + id + "_reset\",\"stat_t\":\"" + b + "/diagnostics\",\"val_tpl\":\"{{ value_json.reset_reason }}\"" + avty + ",\"ic\":\"mdi:restart-alert\"" + diag;
            publishConfig("sensor", "reset", p);
            return true;
        case 11:
            p = "{\"name\":\"IP Address\",\"uniq_id\":\"" + id + "_ip\",\"stat_t\":\"" + b + "/diagnostics\",\"val_tpl\":\"{{ value_json.ip }}\"" + avty + ",\"ic\":\"mdi:ip-network\"" + diag;
            publishConfig("sensor", "ip", p);
            return true;
        case 12:
            p = "{\"name\":\"NFC Reader\",\"uniq_id\":\"" + id + "_pn532\",\"stat_t\":\"" + b + "/diagnostics\",\"val_tpl\":\"{{ value_json.nfc }}\"" + avty + ",\"dev_cla\":\"connectivity\"" + diag;
            publishConfig("binary_sensor", "pn532", p);
            return true;
        case 13:
            p = "{\"name\":\"Restart\",\"uniq_id\":\"" + id + "_restart\",\"cmd_t\":\"" + b + "/restart\"" + avty + ",\"dev_cla\":\"restart\"" + diag;
            publishConfig("button", "restart", p);
            return true;
        case 14:
            // HA tag scanner: scans show up in Settings > Tags and fire the
            // standard tag_scanned event with the UID as tag_id
            p = "{\"topic\":\"" + b + "/tag/scanned\",\"value_template\":\"{{ value }}\"";
            publishConfig("tag", "tag", p);
            return true;
        case 15: {
            // Event entity with one event type per registered tag
            p = "{\"name\":\"Tag Scanned\",\"uniq_id\":\"" + id + "_event\",\"stat_t\":\"" + b + "/event\"" + avty + ",\"ic\":\"mdi:nfc-tap\",\"event_types\":[";
            TagEntry entry;
            for (uint8_t i = 0; storage.getRegisteredTag(i, entry); i++) {
                p += "\"";
                p += entry.name;
                p += "\",";
            }
            p += "\"unknown\"]";
            publishConfig("event", "event", p);
            return true;
        }
        default:
            return false;
    }
}

void MQTTHandler::publishDiagnostics() {
    char json[192];
    snprintf(json, sizeof(json),
             "{\"uptime\":%lu,\"free_heap\":%u,\"min_free_heap\":%u,\"reset_reason\":\"%s\",\"ip\":\"%s\",\"nfc\":\"%s\",\"nfc_reconnects\":%u}",
             (unsigned long)(esp_timer_get_time() / 1000000ULL),
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
             resetReasonString(), wifiManager.getIP().c_str(),
             nfcHandler.isConnected() ? "ON" : "OFF",
             (unsigned)nfcHandler.getReconnectCount());
    _mqttClient.publish((getBaseTopic() + "/diagnostics").c_str(), json, true);
}

void MQTTHandler::requestTriggerRemoval(const char* name) {
    if (!name || !name[0]) return;
    portENTER_CRITICAL(&_removalMux);
    if (_removalCount < REMOVAL_QUEUE_SIZE) {
        strlcpy(_removalQueue[_removalCount], name, sizeof(_removalQueue[0]));
        _removalCount++;
    }
    portEXIT_CRITICAL(&_removalMux);
}

void MQTTHandler::processTriggerRemovals() {
    char name[32];
    portENTER_CRITICAL(&_removalMux);
    if (_removalCount == 0) {
        portEXIT_CRITICAL(&_removalMux);
        return;
    }
    _removalCount--;
    strlcpy(name, _removalQueue[_removalCount], sizeof(name));
    portEXIT_CRITICAL(&_removalMux);

    // An empty retained config removes the trigger from HA
    String topic = String(MQTT_DISCOVERY_PREFIX) + "/device_automation/nfcr_" + _deviceId + "_" + name + "/config";
    _mqttClient.publish(topic.c_str(), "", true);
    Serial.printf("[MQTT] Removed trigger: %s\n", name);
}

void MQTTHandler::removeDiscovery() {
    String pre = String(MQTT_DISCOVERY_PREFIX);
    String id = "nfcr_" + _deviceId;
    static const char* const entities[] = {
        "sensor/%s_uid", "binary_sensor/%s_tag", "sensor/%s_wifi", "switch/%s_night",
        "device_automation/%s_scan", "binary_sensor/%s_update", "sensor/%s_latest_ver",
        "sensor/%s_current_ver", "sensor/%s_uptime", "sensor/%s_heap", "sensor/%s_reset",
        "sensor/%s_ip", "binary_sensor/%s_pn532", "button/%s_restart", "tag/%s_tag", "event/%s_event"
    };
    char path[64];
    for (const char* e : entities) {
        snprintf(path, sizeof(path), e, id.c_str());
        _mqttClient.publish((pre + "/" + path + "/config").c_str(), "", true);
        _mqttClient.loop();
    }

    _discoveryPublished = false;
    Serial.println("[MQTT] Discovery removed");
}

void MQTTHandler::removeOldTagTriggers(const char* uid) {
    if (!_mqttClient.connected() || !uid) return;

    // Convert UID like "5C:9E:35:4A" to "5C_9E_35_4A" (format used in v1.3.0)
    String safeUid = String(uid);
    safeUid.replace(":", "_");

    String pre = String(MQTT_DISCOVERY_PREFIX);
    String id = "nfcr_" + _deviceId;

    // Remove old UID-based trigger (from v1.3.0) - format was: nfcr_<id>_tag_<safeUid>
    String topic = pre + "/device_automation/" + id + "_tag_" + safeUid + "/config";
    _mqttClient.publish(topic.c_str(), "", true);
    _mqttClient.loop();
    yield();

    Serial.printf("[MQTT] Removed old trigger: %s (topic: %s)\n", uid, topic.c_str());
}

void MQTTHandler::publishState() {
    if (_publishState == MqttPublishState::IDLE) {
        _publishState = MqttPublishState::STATE_LAST_UID;
        _lastPublishStep = millis();
    }
}

void MQTTHandler::publishAvailability(bool online) {
    _mqttClient.publish((getBaseTopic() + "/availability").c_str(), online ? "online" : "offline", true);
}

String MQTTHandler::getBaseTopic() {
    return String(MQTT_TOPIC_PREFIX) + "_" + _deviceId;
}

void MQTTHandler::publishNamedTagTriggerDiscovery(const char* name) {
    if (!name || strlen(name) == 0) return;

    // Device trigger for a named tag, shows as "tag_scanned <name>" in HA.
    // Names are sanitized to [A-Za-z0-9_] by the registry, so they are safe
    // in topics and JSON.
    String b = getBaseTopic();
    String p;
    p.reserve(512);
    p = "{\"automation_type\":\"trigger\",\"type\":\"tag_scanned\",\"subtype\":\"";
    p += name;
    p += "\",\"topic\":\"" + b + "/tag/" + name + "\"";
    publishConfig("device_automation", name, p);
}

void MQTTHandler::subscribeToCommands() {
    String base = getBaseTopic();
    _mqttClient.subscribe((base + "/night_mode/set").c_str());
    _mqttClient.subscribe((base + "/restart").c_str());
    Serial.printf("[MQTT] Subscribed to: %s/night_mode/set, %s/restart\n", base.c_str(), base.c_str());
}

void MQTTHandler::mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
    // Convert payload to string
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    Serial.printf("[MQTT] Received: %s = %s\n", topic, message.c_str());

    String topicStr = String(topic);
    if (topicStr.endsWith("/night_mode/set")) {
        bool nightMode = (message == "ON" || message == "on" || message == "1" || message == "true");
        ledController.setNightMode(nightMode);

        // Publish new state back (for HA confirmation)
        if (_mqttInstance && _mqttInstance->isConnected()) {
            String stateTopic = _mqttInstance->getBaseTopic() + "/night_mode";
            _mqttInstance->_mqttClient.publish(stateTopic.c_str(), nightMode ? "ON" : "OFF", true);
        }
    } else if (topicStr.endsWith("/restart") && _mqttInstance) {
        // Restart from loop(), not from inside the PubSubClient callback
        _mqttInstance->_restartRequested = true;
    }
}
