# NFC Reader for Home Assistant

ESP32-C3 SuperMini + PN532 NFC reader with MQTT integration for Home Assistant.

## Features

- **AP Mode on first boot**: `NFC-READER-XXXX` (last 4 hex of MAC)
- **Web interface** for WiFi/MQTT configuration
- **OTA updates** via web interface
- **MQTT** with Home Assistant auto-discovery
- **Device trigger** for tag-specific automations
- **RGB Status LED** with color-coded feedback
- **Night mode** - disable LED via MQTT/web (ideal for bedroom)
- **Scan history** (last 10 scans)

## Hardware

### Requirements

- ESP32-C3 SuperMini
- PN532 NFC/RFID breakout board
- LED (optional) + 330Ω resistor
- Dupont wires

### Pinout

| ESP32-C3 | PN532 | Description |
|----------|-------|-------------|
| 3.3V | VCC | Power |
| GND | GND | Ground |
| GPIO4 | SDA | I2C Data |
| GPIO5 | SCL | I2C Clock |
| GPIO3 | RSTO | Reset (optional) |

| ESP32-C3 | Component | Description |
|----------|-----------|-------------|
| GPIO8 | Built-in | RGB LED (WS2812B) |
| GPIO10 | External | LED Anode (via 330Ω, optional) |

### PN532 DIP Switch (I2C mode)

```
SEL0: OFF
SEL1: ON
```

## Installation

### First time (USB)

```bash
# Build firmware
pio run -e esp32c3_supermini

# Flash firmware
pio run -e esp32c3_supermini -t upload

# Build and flash filesystem
pio run -e esp32c3_supermini -t buildfs
pio run -e esp32c3_supermini -t uploadfs
```

### OTA updates

After initial configuration:

```bash
pio run -e esp32c3_ota -t upload
```

## Configuration

1. Connect to WiFi network `NFC-READER-XXXX` (password: `nfc123`)
2. Open http://192.168.4.1
3. Configure WiFi credentials
4. Configure MQTT broker

## MQTT Topics

| Topic | Description |
|-------|-------------|
| `nfc_reader_xxxx/tag/scanned` | Published on each scan (JSON with UID) |
| `nfc_reader_xxxx/last_uid` | Last scanned UID (retained) |
| `nfc_reader_xxxx/tag_present` | ON/OFF |
| `nfc_reader_xxxx/availability` | online/offline |
| `nfc_reader_xxxx/night_mode` | Night mode status ON/OFF |
| `nfc_reader_xxxx/night_mode/set` | Night mode command (send ON/OFF) |

## Home Assistant Automation

### Trigger on Specific NFC Tag (via Device Trigger)

The NFC Reader registers as a device trigger in Home Assistant. You can create automations that trigger when a specific tag is scanned:

```yaml
automation:
  - alias: "Kids Room Light - NFC Tag"
    trigger:
      - platform: device
        domain: mqtt
        device_id: <your_device_id>  # Find in HA device page
        type: tag_scanned
        subtype: any
    condition:
      - condition: template
        value_template: "{{ trigger.payload_json.uid == '5C:9E:35:4A' }}"
    action:
      - service: light.toggle
        target:
          entity_id: light.kids_room
```

### Multiple Tags with Choose

```yaml
automation:
  - alias: "NFC Tag Actions"
    trigger:
      - platform: device
        domain: mqtt
        device_id: <your_device_id>
        type: tag_scanned
        subtype: any
    action:
      - choose:
          - conditions:
              - condition: template
                value_template: "{{ trigger.payload_json.uid == '5C:9E:35:4A' }}"
            sequence:
              - service: light.turn_on
                target:
                  entity_id: light.bedroom
          - conditions:
              - condition: template
                value_template: "{{ trigger.payload_json.uid == 'AA:BB:CC:DD' }}"
            sequence:
              - service: media_player.play_media
                target:
                  entity_id: media_player.speaker
                data:
                  media_content_id: "good_night_music"
                  media_content_type: "music"
```

### Night Mode (disable LED during sleep)

```yaml
automation:
  - alias: "NFC Reader Night Mode On"
    trigger:
      - platform: time
        at: "20:00:00"
    action:
      - service: switch.turn_on
        target:
          entity_id: switch.nfc_reader_night_mode

  - alias: "NFC Reader Night Mode Off"
    trigger:
      - platform: time
        at: "07:00:00"
    action:
      - service: switch.turn_off
        target:
          entity_id: switch.nfc_reader_night_mode
```

## Default Credentials

| Setting | Value |
|---------|-------|
| AP Password | `nfc123` |
| OTA Password | `nfc-ota` |
| AP IP | `192.168.4.1` |

## License

MIT
