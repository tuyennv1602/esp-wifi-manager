#pragma once
#include <Arduino.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <ESP8266WebServer.h>
#elif defined(ESP32)
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_wifi.h>
#else
#error "This library supports only ESP8266 or ESP32"
#endif

// ===== WebServer instance =====
#if defined(ESP8266)
extern ESP8266WebServer wifiServer;
#elif defined(ESP32)
extern WebServer wifiServer;
#endif

// ===== ESP8266Preferences (emulate Preferences) =====
#if defined(ESP8266)
class ESP8266Preferences
{
public:
    ESP8266Preferences() {}
    bool begin(const char *ns, bool readOnly = false);
    void end();
    bool isKey(const char *key);
    String getString(const char *key, const String &def = "");
    void putString(const char *key, const String &value);
    void clear();

private:
    String filePath;
    String data;
    bool readOnly;
};
#endif

// =================== API ===================

void wifiManagerBegin();
void wifiManagerLoop();
void setAPName(const String &ssidIn);
void setStaticIP(IPAddress ip, IPAddress gw, IPAddress sn);
void setOnWifiConnectedCallback(std::function<void()> cb);
void resetWiFi();
String getMacAddress();
String getIPAddress();
bool isConnectedWifi();
String getConnectedSSID();
String scanWiFi();
