#include "mqtt_handler.h"
#include "config.h"
#include "nfc_handler.h"
#include "wifi_manager.h"
#include "storage.h"
#include "logger.h"
#include "led_controller.h"
#include <ArduinoJson.h>

MQTTHandler mqttHandler;
MQTTHandler* _mqttInstance = nullptr;

void MQTTHandler::begin() {
    _mqttInstance = this;

    // Set socket timeout
    _wifiClient.setTimeout(3000);

    _mqttClient.setClient(_wifiClient);
    _mqttClient.setKeepAlive(NFC_MQTT_KEEPALIVE);
    _mqttClient.setSocketTimeout(3);
    _mqttClient.setBufferSize(1024);

    // Generate unique device ID from MAC
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char id[13];
    snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    _deviceId = id;

    Serial.println("[MQTT] Handler initialized");
}

void MQTTHandler::loop() {
    if (!_mqttClient.connected()) {
        unsigned long now = millis();
        if (now - _lastReconnect >= MQTT_RECONNECT_INTERVAL) {
            _lastReconnect = now;
            if (_host.length() > 0 && wifiManager.isConnected()) {
                Serial.println("[MQTT] Attempting connection...");
                String clientId = "nfc-reader-" + _deviceId;

                _mqttClient.setCallback(mqttCallback);

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
                } else {
                    Serial.printf("[MQTT] Connection failed, rc=%d\n", _mqttClient.state());
                    logger.errorf("MQTT connection failed (rc=%d)", _mqttClient.state());
                }
            }
        }
    } else {
        _mqttClient.loop();

        // Process non-blocking publish state machine
        processPublishStateMachine();

        // Handle state publish requests
        if (_statePublishPending && _publishState == MqttPublishState::IDLE) {
            noInterrupts();
            _statePublishPending = false;
            interrupts();
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
            // Individual tag triggers are published dynamically when tags are scanned
            // This keeps HA UI clean - only shows tags you've actually used
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

        case MqttPublishState::STATE_WIFI:
            _mqttClient.publish((base + "/wifi_signal").c_str(), String(wifiManager.getRSSI()).c_str(), true);
            _publishState = MqttPublishState::STATE_NIGHT_MODE;
            break;

        case MqttPublishState::STATE_NIGHT_MODE:
            _mqttClient.publish((base + "/night_mode").c_str(), ledController.isNightMode() ? "ON" : "OFF", true);
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

    // Publish device trigger discovery for this specific tag
    // This makes the tag appear as a selectable trigger in HA
    publishTagTriggerDiscovery(uid);

    // Publish to tag/scanned topic with UID as payload (for device trigger matching)
    _mqttClient.publish((base + "/tag/scanned").c_str(), uid, false);

    // Also update retained last_uid (for sensor display)
    _mqttClient.publish((base + "/last_uid").c_str(), uid, true);

    // Update tag present
    _mqttClient.publish((base + "/tag_present").c_str(), "ON", true);

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

    String p = "{\"name\":\"Last Scanned UID\",";
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

    String p = "{\"name\":\"Tag Present\",";
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

    String p = "{\"name\":\"WiFi Signal\",";
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

String MQTTHandler::getDeviceJson() {
    StaticJsonDocument<256> device;
    device["identifiers"][0] = "nfc_reader_" + _deviceId;
    device["name"] = "NFC Reader";
    device["model"] = "ESP32-C3 + PN532";
    device["manufacturer"] = "DIY";
    device["sw_version"] = FIRMWARE_VERSION;

    String output;
    serializeJson(device, output);
    return output;
}

void MQTTHandler::publishNightModeSwitchDiscovery() {
    String b = getBaseTopic();
    String devId = "nfc_reader_" + _deviceId;

    // Home Assistant switch entity for night mode
    String p = "{\"name\":\"Night Mode\",";
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
    // Generic "any tag" trigger is no longer published
    // Individual tag triggers are published when tags are scanned
    // This keeps the HA UI clean with only tags you've actually used
}

void MQTTHandler::publishTagTriggerDiscovery(const char* uid) {
    String b = getBaseTopic();
    String devId = "nfc_reader_" + _deviceId;
    String uidStr = String(uid);

    // Create a safe ID from UID (replace : with _)
    String safeUid = uidStr;
    safeUid.replace(":", "_");

    // Home Assistant device trigger for this specific tag
    // Each scanned tag gets its own trigger in the HA automation UI
    String p = "{\"automation_type\":\"trigger\",";
    p += "\"type\":\"tag_scanned\",";
    p += "\"subtype\":\"" + uidStr + "\",";
    p += "\"topic\":\"" + b + "/tag/scanned\",";
    p += "\"payload\":\"" + uidStr + "\",";
    p += "\"dev\":{\"ids\":[\"" + devId + "\"],";
    p += "\"name\":\"NFC Reader\",\"mf\":\"DIY\",\"mdl\":\"ESP32-C3 + PN532\",\"sw\":\"" FIRMWARE_VERSION "\"}}";

    String topic = String(MQTT_DISCOVERY_PREFIX) + "/device_automation/nfcr_" + _deviceId + "_tag_" + safeUid + "/config";
    _mqttClient.publish(topic.c_str(), p.c_str(), true);

    Serial.printf("[MQTT] Published trigger for tag: %s\n", uid);
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
