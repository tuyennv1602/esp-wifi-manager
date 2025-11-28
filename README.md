# ESP_WiFi_Manager

Simple WiFi manager for ESP8266 / ESP32.

Features:
- AP-based configuration portal (web UI) to select and save WiFi networks
- Stores credentials in Preferences (ESP32) / EEPROM-like API for ESP8266
- Connection state machine with retry and fallback to AP mode

Installation:
- Copy the `ESP_WiFi_Manager` folder into your Arduino `libraries` folder, or zip the folder and use Arduino IDE: Sketch -> Include Library -> Add .ZIP Library.

Usage:
```cpp
#include "wifi_manager.h"

void setup() {
  Serial.begin(115200);
  wifiManagerBegin(); // call once in setup
}

void loop() {
  wifiManagerLoop(); // call from loop()
}
```

Examples:
- `examples/BasicExample` demonstrates the minimal sketch to use the library.

License: MIT (see LICENSE file)

Notes:
- This library targets ESP8266 and ESP32 boards.
- If you modify the AP name use `setAPName("MyDevice");` before `wifiManagerBegin()`.

