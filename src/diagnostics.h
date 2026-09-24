#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <Arduino.h>
#include <esp_system.h>

// Human-readable reason for the last reset (shown in the web UI and sent to
// Home Assistant, so silent crashes and brownouts become visible)
inline const char* resetReasonString() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "Power on";
        case ESP_RST_EXT:       return "External reset";
        case ESP_RST_SW:        return "Software restart";
        case ESP_RST_PANIC:     return "Crash (panic)";
        case ESP_RST_INT_WDT:   return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "Task watchdog";
        case ESP_RST_WDT:       return "Watchdog";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
        case ESP_RST_BROWNOUT:  return "Brownout";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "Unknown";
    }
}

#endif // DIAGNOSTICS_H
