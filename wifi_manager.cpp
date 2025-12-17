#include "wifi_manager.h"

#if defined(ESP8266)
extern ESP8266Preferences preferences;
ESP8266WebServer wifiServer(80);
#elif defined(ESP32)
extern Preferences preferences;
WebServer wifiServer(80);
#endif
DNSServer dnsServer;

// ------------------- Config -------------------
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

const char *WIFI_HTML = R"=====(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1">
<title>WiFi Configuration</title><style>
    body { font-family: Arial, sans-serif; background: #efefef; margin: 0; padding: 20px; }
    .container { max-width: 600px; background: white; margin: auto; padding: 25px; border-radius: 12px; box-shadow: 0 3px 10px rgba(0,0,0,0.1); }
    h1 { margin-top: 0; }
    label { font-weight: bold; display:block; margin-top:10px; }
    input[type=text], input[type=password] { width:100%; box-sizing:border-box; padding:14px; margin-top:8px; margin-bottom:14px; border-radius:6px; border:1px solid #aaa; font-size:16px; }
    button { width:100%; padding:14px; font-size:18px; background:#4CAF50; color:#fff; border:none; border-radius:6px; cursor:pointer; }
    button.secondary { background:#eee; color:#000; margin-top:8px; }
    .wifi-item { padding:12px; margin-bottom:8px; background:#f5f5f5; border-radius:6px; border:1px solid #ddd; cursor:pointer; }
</style></head><body><div class="container"><h1>WiFi Configuration</h1>
  <label>WiFi SSID</label><input id="ssid" type="text" placeholder="Enter WiFi name">
  <label>Password</label><input id="pass" type="password" placeholder="Password">
  <button onclick="saveWifi()">Save & Connect</button>
  <button class="secondary" onclick="resetWifi()">Reset Saved WiFi</button>
  <h2>Nearby WiFi</h2><div id="list">Loading...</div></div>
<script>
function renderWifiList(list) {
  const box = document.getElementById("list"); box.innerHTML = "";
  list.sort((a,b)=>b.rssi - a.rssi);
  list.forEach(item=>{
    const d = document.createElement("div"); d.className = "wifi-item";
    d.textContent = item.ssid + " (" + item.rssi + " dBm)";
    d.onclick = ()=> document.getElementById("ssid").value = item.ssid;
    box.appendChild(d);
  });
}
function loadWifi() { fetch("/scan").then(r => r.json()).then(d => renderWifiList(d)).catch(()=> document.getElementById("list").innerText = "Failed to load"); }
function saveWifi() {
  const ssid = document.getElementById("ssid").value; const pass = document.getElementById("pass").value;
  if(!ssid) { alert("SSID required"); return; }
  fetch("/save?ssid="+encodeURIComponent(ssid)+"&pass="+encodeURIComponent(pass)).then(()=> { alert("Saved! Device will reboot."); }).catch(()=> alert("Save failed"));
}
function resetWifi() { if(!confirm("Reset all saved WiFi?")) return; fetch("/reset").then(()=> alert("Reset done")).catch(()=> alert("Reset failed")); }
window.onload = function() { loadWifi(); setInterval(loadWifi, 8000); };
</script></body></html>
)=====";

// ================= Utils =================
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

static String prefGetString(const String &k, const String &d = "")
{
    preferences.begin("wifi", true);
    String v = preferences.getString(k.c_str(), d);
    preferences.end();
    return v;
}
static void prefPutString(const String &k, const String &v)
{
    preferences.begin("wifi", false);
    preferences.putString(k.c_str(), v.c_str());
    preferences.end();
}

void setAPName(const String &s) { apSsid = s; }
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

String scanWiFi()
{
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; ++i)
    {
        if (i)
            json += ",";
        json += "{\"rssi\":" + String(WiFi.RSSI(i)) + ",\"ssid\":\"" + WiFi.SSID(i) + "\"}";
    }
    return json + "]";
}

int getRSSIOf(const String &target)
{
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++)
        if (WiFi.SSID(i) == target)
            return WiFi.RSSI(i);
    return -999;
}

void sortWiFiBySignal()
{
    String s1 = prefGetString("ssid1");
    String p1 = prefGetString("pass1");
    String s2 = prefGetString("ssid2");
    String p2 = prefGetString("pass2");
    if (s1 == "" || s2 == "")
        return;
    if (getRSSIOf(s2) > getRSSIOf(s1))
    {
        prefPutString("ssid1", s2);
        prefPutString("pass1", p2);
        prefPutString("ssid2", s1);
        prefPutString("pass2", p1);
    }
}

void saveWiFi(const String &S, const String &P)
{
    prefPutString("ssid2", prefGetString("ssid1"));
    prefPutString("pass2", prefGetString("pass1"));
    prefPutString("ssid1", S);
    prefPutString("pass1", P);
}

// ================= Logic =================

void startConnectSlot(int slot)
{
    String s = prefGetString(slot == 1 ? "ssid1" : "ssid2");
    String p = prefGetString(slot == 1 ? "pass1" : "pass2");
    if (s == "")
    {
        wifiState = WIFI_FAILED;
        return;
    }
    WiFi.mode(WIFI_AP_STA);
    if (useStaticIP)
        WiFi.config(static_IP, static_gateway, static_subnet);
    WiFi.begin(s.c_str(), p.c_str());
    wifiRetryTimer = millis();
    wifiState = WIFI_CONNECTING;
}

void startAccessPointMode()
{
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(apSsid.c_str());
    dnsServer.start(53, "*", local_IP);
    if (!serverStarted)
    {
        wifiServer.on("/", HTTP_GET, []()
                      { wifiServer.send(200, "text/html", WIFI_HTML); });
        wifiServer.on("/scan", HTTP_GET, []()
                      { wifiServer.send(200, "application/json", scanWiFi()); });
        wifiServer.on("/save", HTTP_GET, []()
                      {
            String s = URLDecode(wifiServer.arg("ssid")); String p = URLDecode(wifiServer.arg("pass"));
            if(s != "") { saveWiFi(s, p); wifiServer.send(200, "text/plain", "OK"); delay(1000); ESP.restart(); } });
        wifiServer.on("/reset", HTTP_GET, []()
                      { preferences.begin("wifi", false); preferences.clear(); preferences.end(); wifiServer.send(200, "text/plain", "OK"); });
        wifiServer.onNotFound([]()
                              { wifiServer.sendHeader("Location", "http://192.168.4.1/", true); wifiServer.send(302, "text/plain", ""); });
        wifiServer.begin();
        serverStarted = true;
    }
    wifiState = WIFI_AP_MODE;
}

void connectWifi()
{
    sortWiFiBySignal();
    currentSlot = 1;
    startConnectSlot(currentSlot);
}

void wifiManagerBegin()
{
    getMacAddress();
    if (prefGetString("ssid1") != "")
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
            if (currentSlot == 1 && prefGetString("ssid2") != "")
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

String getIPAddress() { return WiFi.localIP().toString(); }
bool isConnectedWifi() { return WiFi.status() == WL_CONNECTED; }
String getConnectedSSID() { return isConnectedWifi() ? WiFi.SSID() : ""; }
void resetWiFi()
{
    preferences.begin("wifi", false);
    preferences.clear();
    preferences.end();
}