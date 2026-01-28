# NFC Reader for Home Assistant

ESP32-C3 SuperMini + PN532 NFC reader with MQTT integration for Home Assistant.

## Features

- **AP Mode bij eerste boot**: `NFC-READER-XXXX` (last 4 hex of MAC)
- **Web interface** voor WiFi/MQTT configuratie
- **OTA updates** via web interface
- **MQTT** met Home Assistant auto-discovery
- **Last Scanned UID** sensor voor automations
- **RGB Status LED** met kleur-gecodeerde feedback
- **Night mode** - LED uitschakelen via MQTT/web (ideaal voor slaapkamer)
- **Scan history** (laatste 10 scans)

## Hardware

### Benodigdheden

- ESP32-C3 SuperMini
- PN532 NFC/RFID breakout board
- LED (optioneel) + 330Ω weerstand
- Dupont kabeltjes

### Pinout

| ESP32-C3 | PN532 | Beschrijving |
|----------|-------|--------------|
| 3.3V | VCC | Voeding |
| GND | GND | Ground |
| GPIO4 | SDA | I2C Data |
| GPIO5 | SCL | I2C Clock |
| GPIO3 | RSTO | Reset (optioneel) |

| ESP32-C3 | Component | Beschrijving |
|----------|-----------|--------------|
| GPIO8 | Ingebouwd | RGB LED (WS2812B) |
| GPIO10 | Extern | LED Anode (via 330Ω, optioneel) |

### PN532 DIP Switch (I2C mode)

```
SEL0: OFF
SEL1: ON
```

## Installatie

### Eerste keer (USB)

```bash
# Bouw firmware
pio run -e esp32c3_supermini

# Flash firmware
pio run -e esp32c3_supermini -t upload

# Bouw en flash filesystem
pio run -e esp32c3_supermini -t buildfs
pio run -e esp32c3_supermini -t uploadfs
```

### OTA updates

Na eerste configuratie:

```bash
pio run -e esp32c3_ota -t upload
```

## Configuratie

1. Verbind met WiFi netwerk `NFC-READER-XXXX` (password: `nfc123`)
2. Open http://192.168.4.1
3. Configureer WiFi credentials
4. Configureer MQTT broker

## MQTT Topics

| Topic | Beschrijving |
|-------|--------------|
| `nfc_reader_xxxx/tag/scanned` | Gepubliceerd bij elke scan |
| `nfc_reader_xxxx/last_uid` | Laatste UID (retained) |
| `nfc_reader_xxxx/tag_present` | ON/OFF |
| `nfc_reader_xxxx/availability` | online/offline |
| `nfc_reader_xxxx/night_mode` | Night mode status ON/OFF |
| `nfc_reader_xxxx/night_mode/set` | Night mode command (stuur ON/OFF) |

## Home Assistant Automation

### NFC Tag Toggle

```yaml
automation:
  - alias: "NFC Tag Toggle Light"
    trigger:
      - platform: mqtt
        topic: "nfc_reader_xxxx/tag/scanned"
        payload: "04:A3:2B:1C:5D:6E:7F"
    action:
      - service: light.toggle
        target:
          entity_id: light.woonkamer
```

### Night Mode (LED uit tijdens slaaptijd)

```yaml
automation:
  - alias: "NFC Reader Night Mode Aan"
    trigger:
      - platform: time
        at: "20:00:00"
    action:
      - service: switch.turn_on
        target:
          entity_id: switch.nfc_reader_night_mode

  - alias: "NFC Reader Night Mode Uit"
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
