#include "ota_handler.h"
#include "config.h"
#include "storage.h"
#include <ArduinoOTA.h>

OTAHandler otaHandler;

void OTAHandler::begin() {
    // begin() is called on every WiFi (re)connect; ArduinoOTA must only be
    // initialized once (repeated begin() re-registers mDNS/UDP listeners)
    if (_started) {
        return;
    }
    _started = true;

    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(storage.getOTAPassword());

    ArduinoOTA.onStart([this]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "firmware";
        } else {
            type = "filesystem";
        }
        Serial.println("[OTA] Start updating " + type);

        if (_startCallback) {
            _startCallback();
        }
    });

    ArduinoOTA.onEnd([this]() {
        Serial.println("\n[OTA] Update complete");

        if (_endCallback) {
            _endCallback();
        }
    });

    ArduinoOTA.onProgress([this](unsigned int progress, unsigned int total) {
        int percent = (total > 0) ? ((progress * 100) / total) : 0;
        Serial.printf("[OTA] Progress: %u%%\r", percent);

        if (_progressCallback) {
            _progressCallback(percent);
        }
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA] Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

    ArduinoOTA.begin();
    Serial.println("[OTA] Service started");
}

void OTAHandler::loop() {
    ArduinoOTA.handle();
}

void OTAHandler::onProgress(OTACallback callback) {
    _progressCallback = callback;
}

void OTAHandler::onStart(void (*callback)()) {
    _startCallback = callback;
}

void OTAHandler::onEnd(void (*callback)()) {
    _endCallback = callback;
}
