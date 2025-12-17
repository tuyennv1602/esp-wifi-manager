#include "wifi_manager.h"

#if defined(ESP8266)
extern ESP8266Preferences preferences;
ESP8266WebServer wifiServer(80);
#elif defined(ESP32)
extern Preferences preferences;
WebServer wifiServer(80);
#endif
DNSServer dnsServer;

// ------------------- Config & Globals -------------------
IPAddress local_IP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

static String ssid = "";
static String pass = "";
static String ipAddress = "";
static String macAddress = "";
static String apSsid = "KS_Device";

enum WifiState
{
    WIFI_IDLE,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_FAILED,
    WIFI_AP_MODE
};
static WifiState wifiState = WIFI_IDLE;
static unsigned long wifiRetryTimer = 0;
static const unsigned long WIFI_RETRY_INTERVAL = 15000;

static int currentSlot = 1;
static std::function<void()> onWifiConnectedCallback = nullptr;
static bool useStaticIP = false;
static IPAddress static_IP, static_gateway, static_subnet;
static bool serverStarted = false;

// ================= ESP8266Preferences Implementation =================
#if defined(ESP8266)
bool ESP8266Preferences::begin(const char *ns, bool readOnly)
{
    this->readOnly = readOnly;
    this->filePath = "/" + String(ns) + ".txt";
    if (!LittleFS.begin())
        LittleFS.format();
    if (LittleFS.exists(filePath))
    {
        File f = LittleFS.open(filePath, "r");
        data = f.readString();
        f.close();
    }
    return true;
}
void ESP8266Preferences::end() {}
String ESP8266Preferences::getString(const char *key, const String &def)
{
    int idx = data.indexOf(String(key) + ":");
    if (idx == -1)
        return def;
    int start = idx + strlen(key) + 1;
    int end = data.indexOf("\n", start);
    return data.substring(start, (end == -1) ? data.length() : end);
}
void ESP8266Preferences::putString(const char *key, const String &value)
{
    if (readOnly)
        return;
    String k = String(key) + ":";
    int idx = data.indexOf(k);
    if (idx != -1)
    {
        int end = data.indexOf("\n", idx);
        data.remove(idx, (end == -1 ? data.length() : end + 1) - idx);
    }
    data += k + value + "\n";
    File f = LittleFS.open(filePath, "w");
    f.print(data);
    f.close();
}
void ESP8266Preferences::clear()
{
    data = "";
    LittleFS.remove(filePath);
}
#endif

// ================= Preferences Helpers =================
static String prefGetString(const String &key, const String &def = "")
{
    preferences.begin("wifi", true);
    String v = preferences.getString(key.c_str(), def);
    preferences.end();
    return v;
}
static void prefPutString(const String &key, const String &value)
{
    preferences.begin("wifi", false);
    preferences.putString(key.c_str(), value.c_str());
    preferences.end();
}

// ================= Utils Gốc =================
void setAPName(const String &ssidIn) { apSsid = ssidIn; }
void setStaticIP(IPAddress ip, IPAddress gw, IPAddress sn)
{
    useStaticIP = true;
    static_IP = ip;
    static_gateway = gw;
    static_subnet = sn;
}
void setOnWifiConnectedCallback(std::function<void()> cb) { onWifiConnectedCallback = cb; }

String getMacAddress()
{
    if (macAddress != "")
        return macAddress;
    WiFi.mode(WIFI_STA);
#if defined(ESP8266)
    macAddress = WiFi.macAddress();
#else
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char buf[18];
    sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    macAddress = String(buf);
#endif
    return macAddress;
}

String getIPAddress() { return WiFi.localIP().toString(); }
bool isConnectedWifi() { return WiFi.status() == WL_CONNECTED; }
String getConnectedSSID() { return isConnectedWifi() ? WiFi.SSID() : ""; }

void resetWiFi()
{
    preferences.begin("wifi", false);
    preferences.clear();
    preferences.end();
    Serial.println("WiFi Reset Done.");
}

String URLDecode(const String &str)
{
    String ret = "";
    char c;
    for (size_t i = 0; i < str.length(); ++i)
    {
        c = str.charAt(i);
        if (c == '+')
            ret += ' ';
        else if (c == '%' && i + 2 < str.length())
        {
            ret += (char)strtol(str.substring(i + 1, i + 3).c_str(), nullptr, 16);
            i += 2;
        }
        else
            ret += c;
    }
    return ret;
}

String scanWiFi()
{
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; ++i)
    {
        if (i)
            json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    return json;
}

int getRSSIOf(const String &target)
{
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++)
        if (WiFi.SSID(i) == target)
            return WiFi.RSSI(i);
    return -999;
}

// ================= Logic Kết Nối =================

void startConnectSlot(int slot)
{
    String s = prefGetString(slot == 1 ? "ssid1" : "ssid2", "");
    String p = prefGetString(slot == 1 ? "pass1" : "pass2", "");
    if (s == "")
    {
        wifiState = WIFI_FAILED;
        return;
    }
    Serial.printf("Attempting Slot %d: %s\n", slot, s.c_str());
    WiFi.mode(WIFI_AP_STA);
    if (useStaticIP)
        WiFi.config(static_IP, static_gateway, static_subnet);
    WiFi.begin(s.c_str(), p.c_str());
    wifiRetryTimer = millis();
    wifiState = WIFI_CONNECTING;
}

void startAccessPointMode()
{
    Serial.println("AP Mode Active: " + apSsid);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(apSsid.c_str());
    dnsServer.start(53, "*", local_IP);

    if (!serverStarted)
    {
        wifiServer.on("/", HTTP_GET, []()
                      {
            String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><title>WiFi Setup</title></head><body>"
                          "<h2>Connect to WiFi</h2><form action='/save'>"
                          "SSID: <br><input name='ssid' id='s'><br>"
                          "PASS: <br><input name='pass' type='password'><br><br>"
                          "<input type='submit' value='Save'></form>"
                          "<h4>Nearby:</h4><div id='l'>Scanning...</div>"
                          "<script>fetch('/scan').then(r=>r.json()).then(d=>{let h='';d.forEach(i=>{h+='<p onclick=\"document.getElementById(\\'s\\').value=\\''+i.ssid+'\\'\">'+i.ssid+' ('+i.rssi+')</p>'});document.getElementById('l').innerHTML=h;});</script></body></html>";
            wifiServer.send(200, "text/html", html); });
        wifiServer.on("/scan", HTTP_GET, []()
                      { wifiServer.send(200, "application/json", scanWiFi()); });
        wifiServer.on("/save", HTTP_GET, []()
                      {
            String s = URLDecode(wifiServer.arg("ssid"));
            String p = URLDecode(wifiServer.arg("pass"));
            if (s != "") {
                String oldS = prefGetString("ssid1", "");
                String oldP = prefGetString("pass1", "");
                prefPutString("ssid2", oldS); prefPutString("pass2", oldP);
                prefPutString("ssid1", s); prefPutString("pass1", p);
                wifiServer.send(200, "text/plain", "Saved! Rebooting...");
                delay(1000); ESP.restart();
            } });
        wifiServer.on("/reset", HTTP_GET, []()
                      { resetWiFi(); wifiServer.send(200, "text/plain", "Reset Done"); });
        wifiServer.onNotFound([]()
                              {
            wifiServer.sendHeader("Location", "http://192.168.4.1/", true);
            wifiServer.send(302, "text/plain", ""); });
        wifiServer.begin();
        serverStarted = true;
    }
    wifiState = WIFI_AP_MODE;
}

void connectWifi()
{
    currentSlot = 1;
    startConnectSlot(currentSlot);
}

void wifiManagerBegin()
{
    getMacAddress();
    if (prefGetString("ssid1", "") != "")
        connectWifi();
    else
        startAccessPointMode();
}

void wifiManagerLoop()
{
    if (serverStarted)
    {
        dnsServer.processNextRequest();
        wifiServer.handleClient();
    }
    switch (wifiState)
    {
    case WIFI_CONNECTING:
        if (WiFi.status() == WL_CONNECTED)
        {
            wifiState = WIFI_CONNECTED;
            if (onWifiConnectedCallback)
                onWifiConnectedCallback();
        }
        else if (millis() - wifiRetryTimer > 15000)
        {
            if (currentSlot == 1 && prefGetString("ssid2", "") != "")
            {
                currentSlot = 2;
                startConnectSlot(currentSlot);
            }
            else
            {
                wifiState = WIFI_FAILED;
            }
        }
        break;
    case WIFI_FAILED:
        startAccessPointMode();
        break;
    case WIFI_CONNECTED:
        if (WiFi.status() != WL_CONNECTED)
        {
            wifiState = WIFI_IDLE;
            wifiRetryTimer = millis();
        }
        break;
    case WIFI_IDLE:
        if (millis() - wifiRetryTimer > WIFI_RETRY_INTERVAL)
            wifiManagerBegin();
        break;
    case WIFI_AP_MODE:
        break;
    }
}