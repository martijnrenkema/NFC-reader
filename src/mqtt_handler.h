#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"

// Non-blocking connection states
enum class MqttConnectState {
    IDLE,
    TCP_CONNECTING,
    MQTT_CONNECTING,
    CONNECTED,
    FAILED
};

// Non-blocking publish states
enum class MqttPublishState {
    IDLE,
    // Discovery states
    DISC_LAST_UID,
    DISC_TAG_PRESENT,
    DISC_WIFI_SIGNAL,
    DISC_NIGHT_MODE,
    DISC_TAG_SCANNED_TRIGGER,
    DISC_UPDATE_AVAILABLE,
    DISC_LATEST_VERSION,
    DISC_CURRENT_VERSION,
    DISC_DONE,
    // State publish states
    STATE_LAST_UID,
    STATE_TAG_PRESENT,
    STATE_WIFI,
    STATE_NIGHT_MODE,
    STATE_UPDATE,
    STATE_DONE
};

class MQTTHandler {
public:
    void begin();
    void loop();

    // Connection
    void connect(const char* host, uint16_t port, const char* user, const char* password);
    void disconnect();
    bool isConnected();

    // Home Assistant Discovery (non-blocking)
    void publishDiscovery();
    void removeDiscovery();
    void removeOldTagTriggers(const char* uid);  // Remove UID-based triggers from old version

    // State publishing (non-blocking)
    void publishState();
    void publishAvailability(bool online);

    // Publish tag scanned event
    void publishTagScanned(const char* uid);

    // Request state publish (safe to call from any context)
    // Note: volatile bool is sufficient for single-core ESP32-C3
    // Do NOT use noInterrupts() - it breaks RMT/WiFi queues
    void requestStatePublish() {
        _statePublishPending = true;
    }

    // Request discovery publish (safe to call from any context, e.g. the
    // async webserver task - the actual publish happens in loop())
    void requestDiscoveryPublish() {
        _discoveryRequested = true;
    }

    // Suspend/resume MQTT (safe to call from any context).
    // PubSubClient is NOT thread-safe: the OTA upload handlers run in the
    // async webserver task and must not call disconnect() directly while the
    // main loop is inside _mqttClient.loop().
    void requestSuspend() { _suspendRequested = true; }
    void resume() { _suspendRequested = false; }

private:
    WiFiClient _wifiClient;
    PubSubClient _mqttClient;

    String _host;
    uint16_t _port = 1883;
    String _user;
    String _password;
    String _deviceId;

    unsigned long _lastReconnect = 0;
    unsigned long _lastStatePublish = 0;
    unsigned long _lastPublishStep = 0;
    bool _discoveryPublished = false;
    volatile bool _statePublishPending = false;
    volatile bool _discoveryRequested = false;
    volatile bool _suspendRequested = false;
    bool _suspended = false;
    // Reconnect backoff: doubles on failure up to 60s, resets on success.
    // Keeps the (blocking) TCP connect from stalling the loop every 5s when
    // the broker is unreachable.
    unsigned long _reconnectInterval = MQTT_RECONNECT_INTERVAL;

    // Non-blocking connection state machine
    MqttConnectState _connectState = MqttConnectState::IDLE;
    unsigned long _connectStartTime = 0;
    static const unsigned long TCP_CONNECT_TIMEOUT = 2000;  // 2 second TCP timeout
    static const unsigned long MQTT_CONNECT_TIMEOUT = 5000; // 5 second MQTT timeout
    void processConnectStateMachine();

    // Non-blocking publish state machine
    MqttPublishState _publishState = MqttPublishState::IDLE;
    static const unsigned long PUBLISH_STEP_DELAY = 50;

    void processPublishStateMachine();

    void publishLastUIDSensorDiscovery();
    void publishTagPresentBinarySensorDiscovery();
    void publishWiFiSensorDiscovery();
    void publishNightModeSwitchDiscovery();
    void publishTagScannedTriggerDiscovery();
    void publishNamedTagTriggerDiscovery(const char* name);  // Named tag trigger
    void publishUpdateAvailableBinarySensorDiscovery();
    void publishLatestVersionSensorDiscovery();
    void publishCurrentVersionSensorDiscovery();

    void subscribeToCommands();
    static void mqttCallback(char* topic, uint8_t* payload, unsigned int length);

    String getBaseTopic();
};

extern MQTTHandler mqttHandler;

#endif // MQTT_HANDLER_H
