#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"

// Non-blocking publish states
enum class MqttPublishState {
    IDLE,
    // Discovery states
    DISC_LAST_UID,
    DISC_TAG_PRESENT,
    DISC_WIFI_SIGNAL,
    DISC_NIGHT_MODE,
    DISC_TAG_SCANNED_TRIGGER,
    DISC_DONE,
    // State publish states
    STATE_LAST_UID,
    STATE_TAG_PRESENT,
    STATE_WIFI,
    STATE_NIGHT_MODE,
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

    // State publishing (non-blocking)
    void publishState();
    void publishAvailability(bool online);

    // Publish tag scanned event
    void publishTagScanned(const char* uid);

    // Request state publish (safe to call from any context)
    void requestStatePublish() {
        noInterrupts();
        _statePublishPending = true;
        interrupts();
    }

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

    // Non-blocking state machine
    MqttPublishState _publishState = MqttPublishState::IDLE;
    static const unsigned long PUBLISH_STEP_DELAY = 50;

    void processPublishStateMachine();

    void publishLastUIDSensorDiscovery();
    void publishTagPresentBinarySensorDiscovery();
    void publishWiFiSensorDiscovery();
    void publishNightModeSwitchDiscovery();
    void publishTagScannedTriggerDiscovery();

    void subscribeToCommands();
    static void mqttCallback(char* topic, uint8_t* payload, unsigned int length);

    String getBaseTopic();
    String getDeviceJson();
};

extern MQTTHandler mqttHandler;

#endif // MQTT_HANDLER_H
