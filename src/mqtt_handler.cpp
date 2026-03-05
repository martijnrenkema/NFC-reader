#include "mqtt_handler.h"
#include "config.h"
#include "nfc_handler.h"
#include "wifi_manager.h"
#include "storage.h"
#include "logger.h"
#include "led_controller.h"
#include "update_checker.h"
#include <ArduinoJson.h>

MQTTHandler mqttHandler;
MQTTHandler* _mqttInstance = nullptr;

void MQTTHandler::begin() {
    _mqttInstance = this;

    // Set socket timeouts for better responsiveness
    _wifiClient.setTimeout(2000);  // 2 second TCP timeout

    _mqttClient.setClient(_wifiClient);
    _mqttClient.setKeepAlive(NFC_MQTT_KEEPALIVE);
    _mqttClient.setSocketTimeout(5);  // 5 second MQTT socket timeout (handshake needs time)
    _mqttClient.setBufferSize(1024);

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
                if (now - _lastReconnect >= MQTT_RECONNECT_INTERVAL) {
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
                    _publishState = MqttPublishState::DISC_LAST_UID;
                    _lastPublishStep = millis();
                    Serial.println("[MQTT] Starting discovery publish...");
                } else {
                    _publishState = MqttPublishState::STATE_LAST_UID;
                    _lastPublishStep = millis();
                }
                _connectState = MqttConnectState::CONNECTED;
            } else {
                Serial.printf("[MQTT] Connection failed, rc=%d\n", _mqttClient.state());
                logger.errorf("MQTT connection failed (rc=%d)", _mqttClient.state());
                _wifiClient.stop();
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
    // Process non-blocking connection state machine
    processConnectStateMachine();

    if (_mqttClient.connected()) {
        _mqttClient.loop();

        // Process non-blocking publish state machine
        processPublishStateMachine();

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
        // Discovery states
        case MqttPublishState::DISC_LAST_UID:
            publishLastUIDSensorDiscovery();
            _publishState = MqttPublishState::DISC_TAG_PRESENT;
            break;

        case MqttPublishState::DISC_TAG_PRESENT:
            publishTagPresentBinarySensorDiscovery();
            _publishState = MqttPublishState::DISC_WIFI_SIGNAL;
            break;

        case MqttPublishState::DISC_WIFI_SIGNAL:
            publishWiFiSensorDiscovery();
            _publishState = MqttPublishState::DISC_NIGHT_MODE;
            break;

        case MqttPublishState::DISC_NIGHT_MODE:
            publishNightModeSwitchDiscovery();
            _publishState = MqttPublishState::DISC_TAG_SCANNED_TRIGGER;
            break;

        case MqttPublishState::DISC_TAG_SCANNED_TRIGGER:
            // Publish generic tag_scanned trigger (always available)
            publishTagScannedTriggerDiscovery();
            // Named tag triggers will be published when tags are scanned
            // This avoids blocking NVS reads during discovery
            _publishState = MqttPublishState::DISC_UPDATE_AVAILABLE;
            break;

        case MqttPublishState::DISC_UPDATE_AVAILABLE:
            publishUpdateAvailableBinarySensorDiscovery();
            _publishState = MqttPublishState::DISC_LATEST_VERSION;
            break;

        case MqttPublishState::DISC_LATEST_VERSION:
            publishLatestVersionSensorDiscovery();
            _publishState = MqttPublishState::DISC_CURRENT_VERSION;
            break;

        case MqttPublishState::DISC_CURRENT_VERSION:
            publishCurrentVersionSensorDiscovery();
            _publishState = MqttPublishState::DISC_DONE;
            break;

        case MqttPublishState::DISC_DONE:
            Serial.println("[MQTT] Discovery published");
            _discoveryPublished = true;
            _publishState = MqttPublishState::STATE_LAST_UID;
            break;

        // State publish states
        case MqttPublishState::STATE_LAST_UID:
            _mqttClient.publish((base + "/last_uid").c_str(), nfcHandler.getLastUID(), true);
            _publishState = MqttPublishState::STATE_TAG_PRESENT;
            break;

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
            // Publish update-related states
            const UpdateInfo& info = updateChecker.getInfo();
            _mqttClient.publish((base + "/update_available").c_str(), info.available ? "ON" : "OFF", true);
            _mqttClient.publish((base + "/latest_version").c_str(), info.latestVersion[0] ? info.latestVersion : "unknown", true);
            _mqttClient.publish((base + "/current_version").c_str(), info.currentVersion, true);
            _publishState = MqttPublishState::STATE_DONE;
            break;
        }

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

    Serial.printf("[MQTT] Configured: %s:%d\n", host, port);
}

void MQTTHandler::disconnect() {
    if (_mqttClient.connected()) {
        publishAvailability(false);
        _mqttClient.disconnect();
    }
}

bool MQTTHandler::isConnected() {
    return _mqttClient.connected();
}

void MQTTHandler::publishTagScanned(const char* uid) {
    if (!_mqttClient.connected()) return;

    String base = getBaseTopic();

    // Publish to tag/scanned topic with UID as payload
    // HA device trigger will fire, user checks UID in automation condition
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
    if (storage.getTagName(uid, tagName, sizeof(tagName))) {
        // Publish to named tag topic for HA device trigger
        String namedTopic = base + "/tag/" + String(tagName);
        _mqttClient.publish(namedTopic.c_str(), uid, false);
        _mqttClient.loop();
        yield();

        // Publish discovery for this named trigger (if not already)
        publishNamedTagTriggerDiscovery(tagName);
        _mqttClient.loop();
        yield();

        Serial.printf("[MQTT] Published named tag: %s (%s)\n", tagName, uid);
    }

    Serial.printf("[MQTT] Published tag: %s\n", uid);
}

void MQTTHandler::publishDiscovery() {
    if (_publishState == MqttPublishState::IDLE) {
        _publishState = MqttPublishState::DISC_LAST_UID;
        _lastPublishStep = millis();
        Serial.println("[MQTT] Publishing Home Assistant discovery...");
    }
}

void MQTTHandler::publishLastUIDSensorDiscovery() {
    String b = getBaseTopic();
    String devId = "nfc_reader_" + _deviceId;

    String p;
    p.reserve(320);
    p = "{\"name\":\"Last Scanned UID\",";
    p += "\"uniq_id\":\"nfcr_" + _deviceId + "_uid\",";
    p += "\"stat_t\":\"" + b + "/last_uid\",";
    p += "\"avty_t\":\"" + b + "/availability\",";
    p += "\"ic\":\"mdi:nfc\",";
    p += "\"dev\":{\"ids\":[\"" + devId + "\"],";
    p += "\"name\":\"NFC Reader\",\"mf\":\"DIY\",\"mdl\":\"ESP32-C3 + PN532\",\"sw\":\"" FIRMWARE_VERSION "\"}}";

    String topic = String(MQTT_DISCOVERY_PREFIX) + "/sensor/nfcr_" + _deviceId + "_uid/config";
    _mqttClient.publish(topic.c_str(), p.c_str(), true);
}

void MQTTHandler::publishTagPresentBinarySensorDiscovery() {
    String b = getBaseTopic();
    String devId = "nfc_reader_" + _deviceId;

    String p;
    p.reserve(300);
    p = "{\"name\":\"Tag Present\",";
    p += "\"uniq_id\":\"nfcr_" + _deviceId + "_tag\",";
    p += "\"stat_t\":\"" + b + "/tag_present\",";
    p += "\"avty_t\":\"" + b + "/availability\",";
    p += "\"dev_cla\":\"presence\",";
    p += "\"ic\":\"mdi:nfc-variant\",";
    p += "\"dev\":{\"ids\":[\"" + devId + "\"]}}";

    String topic = String(MQTT_DISCOVERY_PREFIX) + "/binary_sensor/nfcr_" + _deviceId + "_tag/config";
    _mqttClient.publish(topic.c_str(), p.c_str(), true);
}

void MQTTHandler::publishWiFiSensorDiscovery() {
    String b = getBaseTopic();
    String devId = "nfc_reader_" + _deviceId;

    String p;
    p.reserve(320);
    p = "{\"name\":\"WiFi Signal\",";
    p += "\"uniq_id\":\"nfcr_" + _deviceId + "_wifi\",";
    p += "\"stat_t\":\"" + b + "/wifi_signal\",";
    p += "\"avty_t\":\"" + b + "/availability\",";
    p += "\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",";
    p += "\"ent_cat\":\"diagnostic\",";
    p += "\"dev\":{\"ids\":[\"" + devId + "\"]}}";

    String topic = String(MQTT_DISCOVERY_PREFIX) + "/sensor/nfcr_" + _deviceId + "_wifi/config";
    _mqttClient.publish(topic.c_str(), p.c_str(), true);
}

void MQTTHandler::removeDiscovery() {
    String pre = String(MQTT_DISCOVERY_PREFIX);
    String id = "nfcr_" + _deviceId;

    _mqttClient.publish((pre + "/sensor/" + id + "_uid/config").c_str(), "", true);
    _mqttClient.publish((pre + "/binary_sensor/" + id + "_tag/config").c_str(), "", true);
    _mqttClient.publish((pre + "/sensor/" + id + "_wifi/config").c_str(), "", true);

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

void MQTTHandler::publishNightModeSwitchDiscovery() {
    String b = getBaseTopic();
    String devId = "nfc_reader_" + _deviceId;

    // Home Assistant switch entity for night mode
    String p;
    p.reserve(320);
    p = "{\"name\":\"Night Mode\",";
    p += "\"uniq_id\":\"nfcr_" + _deviceId + "_night\",";
    p += "\"stat_t\":\"" + b + "/night_mode\",";
    p += "\"cmd_t\":\"" + b + "/night_mode/set\",";
    p += "\"avty_t\":\"" + b + "/availability\",";
    p += "\"ic\":\"mdi:weather-night\",";
    p += "\"dev\":{\"ids\":[\"" + devId + "\"]}}";

    String topic = String(MQTT_DISCOVERY_PREFIX) + "/switch/nfcr_" + _deviceId + "_night/config";
    _mqttClient.publish(topic.c_str(), p.c_str(), true);
}

void MQTTHandler::publishTagScannedTriggerDiscovery() {
    String b = getBaseTopic();
    String devId = "nfc_reader_" + _deviceId;

    // Single device trigger for tag scanned events
    // Fires on ANY tag - user specifies UID in automation condition:
    //   condition: "{{ trigger.payload == 'C3:7B:70:19' }}"
    String p;
    p.reserve(350);
    p = "{\"automation_type\":\"trigger\",";
    p += "\"type\":\"tag_scanned\",";
    p += "\"subtype\":\"nfc\",";
    p += "\"topic\":\"" + b + "/tag/scanned\",";
    p += "\"dev\":{\"ids\":[\"" + devId + "\"],";
    p += "\"name\":\"NFC Reader\",\"mf\":\"DIY\",\"mdl\":\"ESP32-C3 + PN532\",\"sw\":\"" FIRMWARE_VERSION "\"}}";

    String topic = String(MQTT_DISCOVERY_PREFIX) + "/device_automation/nfcr_" + _deviceId + "_scan/config";
    _mqttClient.publish(topic.c_str(), p.c_str(), true);
}

void MQTTHandler::publishNamedTagTriggerDiscovery(const char* name) {
    if (!name || strlen(name) == 0) return;

    String b = getBaseTopic();
    String devId = "nfc_reader_" + _deviceId;
    String safeName = String(name);

    // Device trigger for named tag
    // Shows as "tag_scanned <name>" in HA device triggers
    String p;
    p.reserve(350);
    p = "{\"automation_type\":\"trigger\",";
    p += "\"type\":\"tag_scanned\",";
    p += "\"subtype\":\"" + safeName + "\",";
    p += "\"topic\":\"" + b + "/tag/" + safeName + "\",";
    p += "\"dev\":{\"ids\":[\"" + devId + "\"],";
    p += "\"name\":\"NFC Reader\",\"mf\":\"DIY\",\"mdl\":\"ESP32-C3 + PN532\",\"sw\":\"" FIRMWARE_VERSION "\"}}";

    String topic = String(MQTT_DISCOVERY_PREFIX) + "/device_automation/nfcr_" + _deviceId + "_" + safeName + "/config";
    _mqttClient.publish(topic.c_str(), p.c_str(), true);
    Serial.printf("[MQTT] Published named trigger: %s\n", name);
}

void MQTTHandler::subscribeToCommands() {
    String topic = getBaseTopic() + "/night_mode/set";
    _mqttClient.subscribe(topic.c_str());
    Serial.printf("[MQTT] Subscribed to: %s\n", topic.c_str());
}

void MQTTHandler::mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
    // Convert payload to string
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    Serial.printf("[MQTT] Received: %s = %s\n", topic, message.c_str());

    // Check if it's the night mode command
    String topicStr = String(topic);
    if (topicStr.endsWith("/night_mode/set")) {
        bool nightMode = (message == "ON" || message == "on" || message == "1" || message == "true");
        ledController.setNightMode(nightMode);

        // Publish new state back (for HA confirmation)
        if (_mqttInstance && _mqttInstance->isConnected()) {
            String stateTopic = _mqttInstance->getBaseTopic() + "/night_mode";
            _mqttInstance->_mqttClient.publish(stateTopic.c_str(), nightMode ? "ON" : "OFF", true);
        }
    }
}

void MQTTHandler::publishUpdateAvailableBinarySensorDiscovery() {
    String b = getBaseTopic();
    String devId = "nfc_reader_" + _deviceId;

    String p;
    p.reserve(320);
    p = "{\"name\":\"Update Available\",";
    p += "\"uniq_id\":\"nfcr_" + _deviceId + "_update\",";
    p += "\"stat_t\":\"" + b + "/update_available\",";
    p += "\"avty_t\":\"" + b + "/availability\",";
    p += "\"dev_cla\":\"update\",";
    p += "\"ent_cat\":\"diagnostic\",";
    p += "\"ic\":\"mdi:package-up\",";
    p += "\"dev\":{\"ids\":[\"" + devId + "\"]}}";

    String topic = String(MQTT_DISCOVERY_PREFIX) + "/binary_sensor/nfcr_" + _deviceId + "_update/config";
    _mqttClient.publish(topic.c_str(), p.c_str(), true);
}

void MQTTHandler::publishLatestVersionSensorDiscovery() {
    String b = getBaseTopic();
    String devId = "nfc_reader_" + _deviceId;

    String p;
    p.reserve(300);
    p = "{\"name\":\"Latest Version\",";
    p += "\"uniq_id\":\"nfcr_" + _deviceId + "_latest_ver\",";
    p += "\"stat_t\":\"" + b + "/latest_version\",";
    p += "\"avty_t\":\"" + b + "/availability\",";
    p += "\"ent_cat\":\"diagnostic\",";
    p += "\"ic\":\"mdi:new-box\",";
    p += "\"dev\":{\"ids\":[\"" + devId + "\"]}}";

    String topic = String(MQTT_DISCOVERY_PREFIX) + "/sensor/nfcr_" + _deviceId + "_latest_ver/config";
    _mqttClient.publish(topic.c_str(), p.c_str(), true);
}

void MQTTHandler::publishCurrentVersionSensorDiscovery() {
    String b = getBaseTopic();
    String devId = "nfc_reader_" + _deviceId;

    String p;
    p.reserve(300);
    p = "{\"name\":\"Current Version\",";
    p += "\"uniq_id\":\"nfcr_" + _deviceId + "_current_ver\",";
    p += "\"stat_t\":\"" + b + "/current_version\",";
    p += "\"avty_t\":\"" + b + "/availability\",";
    p += "\"ent_cat\":\"diagnostic\",";
    p += "\"ic\":\"mdi:tag\",";
    p += "\"dev\":{\"ids\":[\"" + devId + "\"]}}";

    String topic = String(MQTT_DISCOVERY_PREFIX) + "/sensor/nfcr_" + _deviceId + "_current_ver/config";
    _mqttClient.publish(topic.c_str(), p.c_str(), true);
}
