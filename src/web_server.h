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

    // Deferred action flags
    bool _pendingWifiConnect = false;
    String _pendingWifiSsid;
    String _pendingWifiPassword;
    bool _pendingMqttConnect = false;
    String _pendingMqttHost;
    uint16_t _pendingMqttPort = 1883;
    String _pendingMqttUser;
    String _pendingMqttPassword;
    bool _pendingReset = false;
    bool _pendingRestart = false;
    unsigned long _pendingActionTime = 0;

    void setupRoutes();
    void handleStatus(AsyncWebServerRequest* request);
    void handleSaveWifi(AsyncWebServerRequest* request);
    void handleSaveMqtt(AsyncWebServerRequest* request);
    void handleReset(AsyncWebServerRequest* request);
    void handleSavePasswords(AsyncWebServerRequest* request);
    void handleGetPasswords(AsyncWebServerRequest* request);
    void handleScanHistory(AsyncWebServerRequest* request);
};

extern WebServer webServer;

#endif // WEB_SERVER_H
