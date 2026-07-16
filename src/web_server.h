#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "config.h"

class WebServer {
public:
    void begin();
    void loop();
    void stop();

    // Callback for settings changes
    typedef void (*SettingsCallback)();
    void onSettingsChanged(SettingsCallback callback);

private:
    AsyncWebServer* _server = nullptr;
    SettingsCallback _settingsCallback = nullptr;

    // Deferred action flags. Written by HTTP handlers (async webserver task),
    // consumed by loop() (main task). Fixed buffers + spinlock instead of
    // String to avoid cross-task heap races.
    portMUX_TYPE _pendingMux = portMUX_INITIALIZER_UNLOCKED;
    volatile bool _pendingWifiConnect = false;
    char _pendingWifiSsid[64] = {0};
    char _pendingWifiPassword[64] = {0};
    volatile bool _pendingMqttConnect = false;
    char _pendingMqttHost[64] = {0};
    uint16_t _pendingMqttPort = 1883;
    char _pendingMqttUser[32] = {0};
    char _pendingMqttPassword[64] = {0};
    volatile bool _pendingReset = false;
    volatile bool _pendingRestart = false;
    volatile unsigned long _pendingActionTime = 0;
    // Deferred MQTT trigger cleanup (PubSubClient may only be used from the
    // main task)
    volatile bool _pendingTriggerCleanup = false;
    char _pendingCleanupUid[32] = {0};

    void setupRoutes();
    void handleStatus(AsyncWebServerRequest* request);
    void handleSaveWifi(AsyncWebServerRequest* request);
    void handleSaveMqtt(AsyncWebServerRequest* request);
    void handleReset(AsyncWebServerRequest* request);
    void handleSavePasswords(AsyncWebServerRequest* request);
    void handleGetPasswords(AsyncWebServerRequest* request);
    void handleScanHistory(AsyncWebServerRequest* request);
    void handleUpdateCheck(AsyncWebServerRequest* request);
    void handleUpdateStatus(AsyncWebServerRequest* request);
    void handleUpdateInstall(AsyncWebServerRequest* request);
};

extern WebServer webServer;

#endif // WEB_SERVER_H
