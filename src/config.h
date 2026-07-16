#ifndef CONFIG_H
#define CONFIG_H

// ===========================================
// Platform Detection
// ===========================================
#define PLATFORM_ESP32

// ===========================================
// Pin Definitions - ESP32-C3 SuperMini + PN532
// ===========================================

// I2C pins for PN532
#define I2C_SDA_PIN         4       // GPIO4 - I2C SDA
#define I2C_SCL_PIN         5       // GPIO5 - I2C SCL
#define PN532_RESET_PIN     3       // GPIO3 - PN532 RSTO (reset)

// RGB LED (WS2812B) on ESP32-C3 SuperMini
#define RGB_LED_PIN         8       // GPIO8 - Built-in RGB LED

// Status LED (external, optional)
#define LED_PIN             10      // GPIO10 - Status LED (via 330Ω resistor)

// ===========================================
// NFC Settings
// ===========================================
#define NFC_DEBOUNCE_MS         3000    // Same tag not read twice within this time (3 sec)
#define NFC_SCAN_HISTORY_SIZE   10      // Number of scans to keep in history
#define NFC_CHECK_INTERVAL_MS   750     // How often to check for tags (750ms - less blocking)

// ===========================================
// WiFi Settings
// ===========================================
#define WIFI_AP_SSID_PREFIX     "NFC-READER-"
#define WIFI_AP_PASSWORD        "nfc123"        // <8 chars = WPA2 minimum not met; AP starts open (see wifi_manager)
#define WIFI_CONNECT_TIMEOUT    30000           // 30 seconds
#define WIFI_RECONNECT_INTERVAL 60000           // 1 minute

// ===========================================
// MQTT Settings
// ===========================================
#define MQTT_TOPIC_PREFIX       "nfc_reader"
#define MQTT_DISCOVERY_PREFIX   "homeassistant"
#define MQTT_RECONNECT_INTERVAL 5000            // 5 seconds
#define NFC_MQTT_KEEPALIVE      60              // seconds

// ===========================================
// Webserver Settings
// ===========================================
#define WEBSERVER_PORT          80

// ===========================================
// OTA Settings
// ===========================================
#define OTA_HOSTNAME            "nfc-reader"
#define OTA_PASSWORD            "nfc-ota"

// ===========================================
// NVS Storage Keys
// ===========================================
#define NVS_NAMESPACE           "nfcreader"
#define NVS_WIFI_SSID           "wifi_ssid"
#define NVS_WIFI_PASS           "wifi_pass"
#define NVS_MQTT_HOST           "mqtt_host"
#define NVS_MQTT_PORT           "mqtt_port"
#define NVS_MQTT_USER           "mqtt_user"
#define NVS_MQTT_PASS           "mqtt_pass"
#define NVS_DEVICE_NAME         "device_name"
#define NVS_OTA_PASSWORD        "ota_pass"
#define NVS_AP_PASSWORD         "ap_pass"
#define NVS_TAG_NAMESPACE       "nfctags"
#define NVS_TAG_COUNT           "tag_count"

// ===========================================
// LED Patterns (ms)
// ===========================================
#define LED_BLINK_FAST          100     // NFC scan detected
#define LED_BLINK_SLOW          500     // AP mode
#define LED_PULSE_INTERVAL      2000    // Idle pulsing

// ===========================================
// Misc
// ===========================================
#define SERIAL_BAUD             115200

// ===========================================
// Firmware Version
// ===========================================
#define FIRMWARE_VERSION        "1.8.0"

// ===========================================
// GitHub Repo (for updates)
// ===========================================
#define UPDATE_GITHUB_REPO      "martijnrenkema/NFC-reader"

// ===========================================
// Auto Update Settings
// ===========================================
#define UPDATE_CHECK_INTERVAL   86400000UL  // 24 hours in milliseconds
#define UPDATE_CHECK_TIMEOUT    15000       // 15 seconds

#endif // CONFIG_H
