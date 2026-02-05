# ESP32 Fibre → 4G Failover (ESP32-C5)

A robust Internet failover system based on ESP32-C5, designed to
automatically switch from a primary Fibre connection to a 4G/LTE backup
when Internet connectivity is lost.

This project is hardware-tested and designed to handle real power outages,
router reboots, and unstable network conditions.

---

## Features

- ESP32-C5 (Wi-Fi 2.4 / 5 GHz)
- Automatic Fibre → 4G failover
- Automatic return to Fibre when Internet is restored
- Fibre router power reset via NC relay
- 4G router power control via relay
- Robust against power outages
- Web interface (status, controls, history)
- LCD 16x2 with scrolling information
- RGB LED status indicator
- Physical control buttons
- Persistent event history

---

## Hardware overview

Main components:

- ESP32-C5 development board
- 2x relay module (LOW = ON)
- 16x2 I2C LCD
- RGB LED (WS2812)
- Stable 5V / 2A power supply

---

## Repository structure
firmware/esp32-c5/ → Arduino firmware
docs/ → User manual and wiring
hardware/ → Schematics, PCB, BOM
images/ → Photos and screenshots

---

## Getting started

1. Flash the firmware to the ESP32-C5
2. Power the device
3. Hold the RESET button for 5 seconds to enter Wi-Fi config mode
4. Connect to:
   - SSID: `Config-Monitor`
   - Password: `12345678`
5. Configure your Fibre Wi-Fi
6. The system will start monitoring and switching automatically

---

## Power supply warning

This system requires a **stable 5V / 2A power supply**.
Low-quality USB power modules may cause instability or resets.

---

## License

Firmware: **GNU GPLv3**  
Copyright © 2026 Serge HAAS

---

## Author

Serge HAAS  
France
