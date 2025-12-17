#pragma once
#include <Arduino.h>
#include <functional>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#elif defined(ESP32)
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <DNSServer.h>
#endif

#if defined(ESP8266)
extern ESP8266WebServer wifiServer;
#elif defined(ESP32)
extern WebServer wifiServer;
#endif
extern DNSServer dnsServer;

#if defined(ESP8266)
class ESP8266Preferences
{
public:
    ESP8266Preferences() {}
    bool begin(const char *ns, bool readOnly = false);
    void end();
    String getString(const char *key, const String &def = "");
    void putString(const char *key, const String &value);
    void clear();

private:
    String filePath;
    String data;
    bool readOnly;
};
extern ESP8266Preferences preferences;
#elif defined(ESP32)
extern Preferences preferences;
#endif

// =================== API GỐC ===================
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
void connectWifi();
void startAccessPointMode();