# NFC Reader for Home Assistant

ESP32-C3 SuperMini + PN532 NFC/RFID reader with MQTT integration for Home Assistant. Scan NFC tags to trigger automations.

<p align="center">
  <img src="images/case.jpg" alt="NFC Reader Case" height="300"/>
  <img src="images/webui.png" alt="Web Interface" height="300"/>
</p>

![Version](https://img.shields.io/badge/Version-1.10.0-brightgreen)
![ESP32-C3](https://img.shields.io/badge/ESP32--C3-Tested-blue)
![PlatformIO](https://img.shields.io/badge/PlatformIO-Build-orange)
![Home Assistant](https://img.shields.io/badge/Home%20Assistant-MQTT-41BDF5)
![License](https://img.shields.io/badge/License-MIT-green)

## Features

- **Home Assistant Integration** - MQTT auto-discovery, device triggers, native HA tags and an event entity
- **Tag Registry** - Name your tags for easy automations (e.g., "Bedroom_Light"), rename, export and import
- **Auto-Update** - Checks GitHub for new releases, one-click install
- **Web Interface** - Light/dark web app for phone and desktop: live scans, tags, settings, diagnostics and firmware
- **Diagnostics** - Uptime, free memory, last reset reason and PN532 status in the web UI and Home Assistant
- **Self-Healing** - The PN532 is re-initialized automatically if it stops responding
- **Night Mode** - Disable LED via MQTT/web (ideal for bedroom)
- **RGB LED Status** - Color-coded feedback for connection state
- **OTA Updates** - Wireless firmware updates via web interface or PlatformIO
- **Scan History** - View last 10 scanned tags

## Quick Start

### Option 1: Pre-built Binaries (Easiest)

1. Download the latest release from [Releases](https://github.com/martijnrenkema/NFC-reader/releases)
2. Flash using esptool (see Installation section)

### Option 2: Build from Source

```bash
# Clone repository
git clone https://github.com/martijnrenkema/NFC-reader.git
cd NFC-reader

# Build firmware
pio run -e esp32c3_supermini

# Build filesystem
pio run -e esp32c3_supermini -t buildfs
```

## Hardware

### Requirements

- ESP32-C3 SuperMini
- PN532 NFC/RFID breakout board (I2C mode)
- Dupont wires

### 3D Printed Case

A compact case is available on MakerWorld:

**[NFC Tag Reader Case on MakerWorld](https://makerworld.com/nl/models/1117728-nfc-tag-reader-esp8266-32-c6-c3-supermini-pn532)**

### Wiring

| ESP32-C3 | PN532 | Function |
|----------|-------|----------|
| 3.3V | VCC | Power |
| GND | GND | Ground |
| GPIO4 | SDA | I2C Data |
| GPIO5 | SCL | I2C Clock |
| GPIO3 | RSTO | Reset (optional) |

| ESP32-C3 | Function |
|----------|----------|
| GPIO8 | Built-in RGB LED (WS2812B) |

### PN532 DIP Switch (I2C mode)

```
SEL0: OFF
SEL1: ON
```

## Installation

### Step 1: Flash Firmware

#### Method A: Using PlatformIO (Recommended)

```bash
# Flash firmware
pio run -e esp32c3_supermini -t upload

# Flash filesystem (web interface)
pio run -e esp32c3_supermini -t uploadfs
```

#### Method B: Using esptool (Pre-built binaries)

> **You must flash TWO files: firmware + filesystem**
>
> | File | Address |
> |------|---------|
> | `firmware.bin` | `0x10000` |
> | `littlefs.bin` | `0x3D0000` |

```bash
# Flash both files
esptool.py --port /dev/cu.usbmodem* --chip esp32c3 --baud 921600 \
  write_flash 0x10000 firmware.bin 0x3D0000 littlefs.bin
```

### Step 2: Initial Setup

1. Power on the device - LED will pulse orange (AP mode)
2. Connect to WiFi network: `NFC-READER-XXXX` (password: `nfcreader`)
3. Open browser: `http://192.168.4.1`
4. Configure your WiFi credentials
5. Device restarts and connects to your network

### Step 3: Configure MQTT

1. Find device IP in your router or use the serial monitor
2. Open web interface
3. Enter MQTT broker settings
4. Device appears automatically in Home Assistant

## Updating Firmware

### Method 1: Automatic Update (v1.7.0+)

The device checks GitHub for updates automatically:
- 2 minutes after boot
- Every 24 hours

To install an update:
1. Open the web interface → **Firmware** tab
2. Click **Check again**
3. Click **Install update** when available
4. Device downloads and installs automatically

<p align="center">
  <img src="images/firmware-update.png" alt="Firmware Update Page" height="400"/>
</p>

### Method 2: Manual Web Upload

1. Download firmware from [Releases](https://github.com/martijnrenkema/NFC-reader/releases)
2. Open the web interface → **Firmware** tab → **Manual upload**
3. Upload `firmware.bin` first, then `littlefs.bin`

Only one update runs at a time: an upload is refused while another update is in progress.

### Method 3: PlatformIO OTA

```bash
pio run -e esp32c3_ota -t upload
```

## Tag Registry

Register up to **50 NFC tags** with friendly names for easy Home Assistant automations.

### How It Works

1. **Scan a tag** on the reader. An unknown tag shows a toast with a **Register** button.
2. Or open the **Tags** tab and click **Use last** to fill in the UID
3. Enter a friendly name (e.g., `Bedroom_Light`, `Goodnight`, `Music_Toggle`)
4. Click **Register tag**. The trigger appears in Home Assistant right away.

Click a registered tag to rename it. Deleting or renaming a tag also removes its old trigger from Home Assistant. Use **Export** / **Import** to back up the registry as JSON (for example before a factory reset).

### Naming Rules

- Names must be **1-31 characters**
- Use **letters, numbers, and underscores** only
- Names become Home Assistant **device trigger subtypes**
- Example: Tag named `Bedroom_Light` creates trigger `tag_scanned` / `Bedroom_Light`

### Benefits

| Feature | Without Registry | With Registry |
|---------|------------------|---------------|
| Home Assistant trigger | Generic `nfc` trigger | Named trigger (e.g., `Bedroom_Light`) |
| Automation setup | Need UID condition template | Direct trigger selection |
| Readability | UID like `C3:7B:70:19` | Friendly name |

## Home Assistant Integration

### MQTT Auto-Discovery

The device automatically appears in Home Assistant when MQTT is configured. No manual setup needed!

### Entities Created

| Entity | Type | Description |
|--------|------|-------------|
| Last Scanned UID | Sensor | Last scanned tag UID |
| Tag Present | Binary Sensor | Tag currently on reader |
| WiFi Signal | Sensor | Signal strength (dBm) |
| Night Mode | Switch | Disable LED |
| Tag Scanned | Event | Fires on every scan; event type is the tag name (or `unknown`) |
| Restart | Button | Restart the reader |
| Update Available | Binary Sensor | New firmware available |
| Latest Version | Sensor | Latest available version |
| Current Version | Sensor | Installed firmware version |
| Uptime | Sensor (diagnostic) | Seconds since boot |
| Free Memory | Sensor (diagnostic) | Free heap in bytes |
| Last Reset Reason | Sensor (diagnostic) | e.g. Power on, Crash (panic), Brownout |
| IP Address | Sensor (diagnostic) | Current IP address |
| NFC Reader | Binary Sensor (diagnostic) | PN532 connected |

The device uses the name set in the web interface and links to the web interface from its Home Assistant device page.

### Home Assistant Tags

Every scan is also sent to Home Assistant's built-in tag system. Scanned tags show up under **Settings → Tags**, where you can name them and use the standard **Tag** trigger:

```yaml
automation:
  - alias: "Tag: Bedroom light"
    trigger:
      - platform: tag
        tag_id: "5C:9E:35:4A"
    action:
      - service: light.toggle
        target:
          entity_id: light.bedroom
```

### Event Entity

The **Tag Scanned** event entity has one event type per registered tag, so no templates are needed:

```yaml
automation:
  - alias: "NFC: Goodnight"
    trigger:
      - platform: state
        entity_id: event.nfc_reader_tag_scanned
        attribute: event_type
        to: "Goodnight"
    action:
      - service: scene.turn_on
        target:
          entity_id: scene.goodnight
```

Note: a `state` trigger on `event_type` doesn't fire when the same tag is scanned twice in a row. For that case, trigger on the entity's state (the event timestamp) and check `trigger.to_state.attributes.event_type` in a condition, or use the device triggers below.

### Device Triggers

| Trigger | Description |
|---------|-------------|
| `tag_scanned` / `nfc` | Fires on any tag scan (UID in payload) |
| `tag_scanned` / `<name>` | Fires when named tag is scanned |

### Automation Examples

#### Method 1: Named Tags (Recommended)

Register tags in the web interface for the easiest automations. The tag name becomes a selectable trigger in Home Assistant:

```yaml
automation:
  - alias: "Bedroom Light - NFC Tag"
    trigger:
      - platform: device
        domain: mqtt
        device_id: <your_device_id>
        type: tag_scanned
        subtype: Bedroom_Light  # Your registered tag name
    action:
      - service: light.toggle
        target:
          entity_id: light.bedroom
```

> **Tip:** In the Home Assistant UI, you can select the trigger directly from a dropdown - no need to type the UID!

#### Method 2: Generic Trigger with UID

For unregistered tags, use the generic `nfc` trigger with a template condition:

```yaml
automation:
  - alias: "NFC Tag Action"
    trigger:
      - platform: device
        domain: mqtt
        device_id: <your_device_id>
        type: tag_scanned
        subtype: nfc  # Generic trigger for any tag
    condition:
      - condition: template
        value_template: "{{ trigger.payload == 'C3:7B:70:19' }}"
    action:
      - service: light.toggle
        target:
          entity_id: light.kids_room
```

#### Multiple Tags in One Automation

Handle multiple unregistered tags with `choose`:

```yaml
automation:
  - alias: "NFC Multi-Tag Actions"
    trigger:
      - platform: device
        domain: mqtt
        device_id: <your_device_id>
        type: tag_scanned
        subtype: nfc
    action:
      - choose:
          - conditions: "{{ trigger.payload == 'C3:7B:70:19' }}"
            sequence:
              - service: light.turn_on
                target:
                  entity_id: light.bedroom
          - conditions: "{{ trigger.payload == '5C:9E:35:4A' }}"
            sequence:
              - service: scene.turn_on
                target:
                  entity_id: scene.movie_time
```

#### Night Mode Automation

```yaml
automation:
  - alias: "NFC Reader Night Mode"
    trigger:
      - platform: time
        at: "22:00:00"
    action:
      - service: switch.turn_on
        target:
          entity_id: switch.nfc_reader_night_mode
```

## MQTT Topics

| Topic | Description |
|-------|-------------|
| `nfc_reader_xxxx/tag/scanned` | Tag scan event (UID as payload) |
| `nfc_reader_xxxx/last_uid` | Last scanned UID (retained) |
| `nfc_reader_xxxx/tag_present` | ON/OFF |
| `nfc_reader_xxxx/availability` | online/offline |
| `nfc_reader_xxxx/night_mode` | Night mode status |
| `nfc_reader_xxxx/night_mode/set` | Night mode command |
| `nfc_reader_xxxx/tag/<name>` | Named tag scan event (UID as payload) |
| `nfc_reader_xxxx/event` | Scan event as JSON: `{"event_type":"<name or unknown>","uid":"..."}` |
| `nfc_reader_xxxx/diagnostics` | Uptime, free memory, reset reason, IP, PN532 status (JSON, retained) |
| `nfc_reader_xxxx/restart` | Command topic: any payload restarts the reader |
| `nfc_reader_xxxx/update_available` | Update available (ON/OFF) |
| `nfc_reader_xxxx/latest_version` | Latest version available |
| `nfc_reader_xxxx/current_version` | Currently installed version |

## LED Status Indicators

| Color | Pattern | Status |
|-------|---------|--------|
| Light Blue | Fast blink | Connecting to WiFi |
| Orange | Slow pulse | AP mode (configuration) |
| Green | Soft pulse | Connected and idle |
| Cyan | Double flash | Tag scanned |
| Cyan | Solid | Tag resting on the reader |
| Red | Fast blink | WiFi disconnected |
| Red | Slow blink | PN532 not responding (retrying) |
| Purple | Fast blink | Firmware update in progress |
| Off | - | Night mode enabled |

## Configuration

### Default Passwords

| Function | Default | Changeable |
|----------|---------|------------|
| WiFi AP | `nfcreader` | Yes |
| OTA Updates | `nfc-ota` | Yes |

Change passwords in the web interface under **Settings → Passwords** (8-63 characters).

## Troubleshooting

### Device won't connect to WiFi
1. Long press reset button or power cycle
2. Connect to `NFC-READER-XXXX` AP
3. Reconfigure WiFi settings at `192.168.4.1`

### NFC reader not detected
1. Check wiring (I2C: GPIO4=SDA, GPIO5=SCL)
2. Verify PN532 DIP switches (SEL0=OFF, SEL1=ON)
3. Check serial monitor for error messages

The reader retries automatically (after 10 seconds, backing off to every 5 minutes), so a loose wire recovers without a reboot. **Settings → Diagnostics** shows how often it had to reconnect.

### Unexpected restarts
Check **Last reset** under **Settings → Diagnostics** (or the *Last Reset Reason* sensor in Home Assistant). *Brownout* usually points at the power supply, *Crash* or *Watchdog* at a firmware problem worth reporting.

### Auto-update shows "No releases found"
- Repository must be public for auto-update to work
- Check your internet connection

### Web interface not loading
If the web files are missing (for example after an interrupted filesystem update), the device serves a simple recovery page where you can upload `littlefs.bin` from the latest release. Alternatively:
- Flash the filesystem: `pio run -t uploadfs`
- Or download `littlefs.bin` from releases and flash to `0x3D0000`

## Building from Source

### Prerequisites
- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- USB-C cable

### Build Commands

```bash
# Build firmware
pio run -e esp32c3_supermini

# Build filesystem
pio run -e esp32c3_supermini -t buildfs

# Upload firmware
pio run -e esp32c3_supermini -t upload

# Upload filesystem
pio run -e esp32c3_supermini -t uploadfs
```

The web interface sources live in `data_src/`. A pre-build script (`scripts/gzip_web.py`) gzips them into `data/` on every build, so there is no manual step.

GitHub Actions builds `firmware.bin` and `littlefs.bin` for every push. Pushing a `v*` tag attaches both to the GitHub release, which is where the auto-updater looks for them.

## Project Structure

```
├── src/
│   ├── main.cpp              # Main entry point
│   ├── config.h              # Pin definitions & settings
│   ├── nfc_handler.*         # PN532 NFC reading
│   ├── led_controller.*      # RGB LED control
│   ├── storage.*             # Settings persistence (NVS)
│   ├── wifi_manager.*        # WiFi connection & AP mode
│   ├── web_server.*          # Web interface + OTA
│   ├── mqtt_handler.*        # MQTT + HA discovery
│   ├── update_checker.*      # GitHub auto-update
│   ├── ota_handler.*         # ArduinoOTA
│   ├── logger.*              # Log buffer, saved to LittleFS
│   └── diagnostics.h         # Reset reason helper
├── data_src/                 # Web interface sources
│   ├── index.html
│   ├── update.html           # Redirects to the Firmware tab
│   ├── style.css
│   └── script.js
├── data/                     # Generated: gzipped web files (LittleFS image)
├── scripts/gzip_web.py       # Pre-build script: data_src/ -> data/
├── .github/workflows/        # CI build + release binaries
├── platformio.ini
└── README.md
```

## Dependencies

- [PubSubClient](https://github.com/knolleary/pubsubclient) - MQTT client
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) - JSON parsing
- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) - Async web server
- [PN532](https://github.com/Seeed-Studio/PN532) - NFC reader library

## License

MIT License - feel free to use and modify.

## Changelog

### v1.10.0
**New web interface, Home Assistant improvements & reliability fixes:**
- **New web interface**: Reader, Tags, Settings and Firmware tabs; bottom tab bar on phones, sidebar on desktop; light/dark mode; toasts instead of pop-ups
- **Tags**: register unknown tags straight from a scan, rename, export/import as JSON, names shown in the scan history
- **Home Assistant**: native HA tags, a *Tag Scanned* event entity, diagnostic sensors (uptime, free memory, reset reason, IP, PN532 status), a Restart button, and the device name + web UI link on the device page
- **Triggers in sync**: a new tag's trigger appears right away; deleted or renamed tags no longer leave triggers behind
- **Fix**: a tag resting on the reader no longer fires again every 3 seconds
- **Fix**: filesystem updates could be corrupted by log writes during the upload
- **Fix**: a web upload could abort a running GitHub update; only one update runs at a time now
- **Fix**: passwords longer than 31 characters were silently cut off; WiFi/MQTT input is now validated
- **Fix**: the idle LED is green again, the retained last UID is no longer wiped at boot, and the device name is used in Home Assistant
- **Reliability**: the PN532 reconnects automatically, fewer log writes to flash, and a recovery page appears if the web files are missing
- **Build**: web sources in `data_src/` are gzipped automatically; GitHub Actions builds the release binaries

### v1.8.0
**Bug Fixes, WiFi Improvements & LittleFS:**
- **LittleFS migration**: Switched from deprecated SPIFFS to LittleFS for better reliability and wear leveling
- **WiFi boost**: Maximum TX power (19.5dBm) and disabled power saving for better range
- **Bug fixes**: LED night mode recovery, UID bounds check, NFC null pointer guard, factory reset now clears tag registry, MQTT heap fragmentation reduction, OTA stability improvements

### v1.7.0
**Automatic Update Checker:**
- **Auto-update from GitHub**: Device checks for updates automatically (2 min after boot, then every 24 hours)
- **One-click install**: Download and install firmware + filesystem directly from GitHub releases
- **MQTT sensors**: New `update_available`, `latest_version`, `current_version` sensors
- **Web UI**: New "Automatic Update" section on firmware update page

### v1.6.0
**Stability & Performance:**
- Major stability improvements for 24/7 operation
- Non-blocking LED animations
- Improved MQTT reconnection handling

### v1.5.0
**Tag Registry:**
- Name your tags for easy Home Assistant automations
- Web interface for tag management

For older versions, see [GitHub Releases](https://github.com/martijnrenkema/NFC-reader/releases).
