# WakeSync Mini
ESP8266-Based WiFi NTP Alarm Clock with OLED UI and Web Control

## Overview

WakeSync Mini is a firmware-driven smart alarm clock built on ESP8266 (NodeMCU).  
It integrates NTP time synchronization, OLED rendering, a web configuration interface, EEPROM persistence, and a dynamic buzzer alert pattern.

The system is designed with fault tolerance for WiFi reconnection and time resynchronization.

---


https://github.com/user-attachments/assets/cf7a8679-086f-41ff-8738-16749753f077


## System Architecture

### Core Modules

- WiFi Connectivity Layer (ESP8266WiFi)
- HTTP Server Interface (ESP8266WebServer)
- NTP Time Synchronization (configTime + time.h)
- OLED Rendering Engine (Adafruit SSD1306 + GFX)
- EEPROM Configuration Storage
- Alarm Trigger State Machine
- Non-blocking Sound Pattern Generator

---

## Functional Flow

1. Boot → Load EEPROM config
2. Connect to WiFi
3. Sync time via NTP
4. Start HTTP server
5. Main loop:
   - Handle web client
   - Maintain WiFi + NTP
   - Evaluate alarm trigger
   - Update buzzer pattern
   - Render OLED

---

## Alarm Logic

- Converts 12-hour user input to 24-hour format
- Trigger guard using minute-based unique key
- Snooze override using epoch timestamp
- Exact second boundary trigger (sec == 0)

---

## Sound Pattern

- 500ms cycle (350ms ON / 150ms OFF)
- Warble sweep between 1200Hz–2500Hz
- Frequency modulation step ~60ms
- Escalation effect via directional sweep

---

## Memory Handling

- AlarmSettings struct stored in EEPROM
- Lightweight CRC validation
- Auto-default fallback on corruption

---

## Hardware Interface

### OLED (I2C)
- SDA → GPIO4 (D2)
- SCL → GPIO5 (D1)
- Address → 0x3C

### Buzzer
- GPIO14 (D5)

---

## Resilience Strategy

- WiFi status check every 5 seconds
- NTP retry every 10 seconds
- Time validity guard via epoch threshold
- Safe reset of alarm state on update

---

## Build Requirements

- ESP8266 Arduino Core
- Adafruit GFX
- Adafruit SSD1306

Board: NodeMCU 1.0 (ESP-12E Module)

---
![WakeSync (1)](https://github.com/user-attachments/assets/1788e277-ab23-49e1-a797-d8c059036b64)
![web app](https://github.com/user-attachments/assets/d80bf8de-9166-4f05-a677-26b72887dc5a)
![WakeSync (2)](https://github.com/user-attachments/assets/6970f1d9-b940-421e-9923-64daba9f9c56)


## Future Improvements

- AsyncWebServer migration
- OTA firmware updates
- RTC backup
- Power-fail recovery
- JSON REST API endpoint
- Mobile app integration








---

License: MIT
