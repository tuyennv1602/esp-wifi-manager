#include "wifi_manager.h"

#if defined(ESP8266)
ESP8266Preferences preferences;
ESP8266WebServer wifiServer(80);
#elif defined(ESP32)
Preferences preferences;
WebServer wifiServer(80);
#endif

// ------------------- Config -------------------
IPAddress local_IP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

// ================= Globals =================
static String ssid = "";
static String pass = "";
static String ipAddress = "";
static String macAddress = "";

enum WifiState
{
    WIFI_IDLE,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_FAILED
};
static WifiState wifiState = WIFI_IDLE;
static unsigned long wifiRetryTimer = 0;
static const unsigned long WIFI_RETRY_INTERVAL = 15000;

static int currentSlot = 1; // slot1 → slot2
static std::function<void()> onWifiConnectedCallback = nullptr;
static String apSsid = "KS_Device";
static bool useStaticIP = false;
static IPAddress static_IP;
static IPAddress static_gateway;
static IPAddress static_subnet;
static bool serverStarted = false;

// ================= Utils & Settings =================

void setAPName(const String &ssidIn)
{
    apSsid = ssidIn;
}

void setStaticIP(IPAddress ip, IPAddress gw, IPAddress sn)
{
    useStaticIP = true;
    static_IP = ip;
    static_gateway = gw;
    static_subnet = sn;
}

void setOnWifiConnectedCallback(std::function<void()> cb)
{
    onWifiConnectedCallback = cb;
}

// Forward declarations for server handlers (registered via registerServerRoutes)
void handleRoot();
void handleScan();
void handleSave();
void handleReset();

// ---------------- Preferences helpers ----------------
// Small helpers to avoid repeating begin()/end() blocks around preferences
static String prefGetString(const String &key, const String &def = "")
{
#if defined(ESP8266)
    preferences.begin("wifi", true);
    String v = preferences.getString(key.c_str(), def);
    preferences.end();
    return v;
#else
    preferences.begin("wifi", true);
    String v = preferences.getString(key.c_str(), def);
    preferences.end();
    return v;
#endif
}

static void prefPutString(const String &key, const String &value)
{
    preferences.begin("wifi", false);
    preferences.putString(key.c_str(), value.c_str());
    preferences.end();
}

static void prefClearAll()
{
    preferences.begin("wifi", false);
    preferences.clear();
    preferences.end();
}

// Register web server routes (idempotent when called once at init)
static void registerServerRoutes()
{
    wifiServer.on("/", HTTP_GET, handleRoot);
    wifiServer.on("/scan", HTTP_GET, handleScan);
    wifiServer.on("/save", HTTP_GET, handleSave);
    wifiServer.on("/save", HTTP_POST, handleSave);
    wifiServer.on("/hotspot-detect.html", HTTP_GET, []()
                  { wifiServer.send(204, "text/plain", ""); });
    wifiServer.on("/generate_204", HTTP_GET, []()
                  { wifiServer.send(204, "text/plain", "Welcome to KStudio"); });
    wifiServer.on("/reset", HTTP_GET, handleReset);
}

// MAC
String getMacAddress()
{
    WiFi.mode(WIFI_STA);
#if defined(ESP8266)
    macAddress = WiFi.macAddress();
#elif defined(ESP32)
    uint8_t mac[6];
    esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, mac);
    char buf[18];
    sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    macAddress = String(buf);
#endif
    Serial.print("MAC: ");
    Serial.println(macAddress);
    return macAddress;
}

String getIPAddress()
{
    return ipAddress;
}

String getConnectedSSID()
{
    if (wifiState == WIFI_CONNECTED)
        return WiFi.SSID();
    return "";
}

bool isConnectedWifi()
{
    return wifiState == WIFI_CONNECTED;
}

void onWifiConnected()
{
    delay(200);
    if (onWifiConnectedCallback)
        onWifiConnectedCallback();
}

// ================= Scan / JSON builder =================

// Returns JSON string array of objects: { "ssid":"...", "rssi":-50 }
String scanWiFi()
{
    String json = "[";
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; ++i)
    {
        if (i)
            json += ",";
        json += "{";
        json += "\"rssi\":" + String(WiFi.RSSI(i));
        json += ",\"ssid\":\"" + WiFi.SSID(i) + "\"";
        json += "}";
    }
    json += "]";
    return json;
}

// RSSI of saved SSID
int getRSSIOf(const String &target)
{
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++)
    {
        if (WiFi.SSID(i) == target)
            return WiFi.RSSI(i);
    }
    return -999;
}

// ================= Preferences (load/save slots) =================

// Load a WiFi slot into global ssid/pass; returns true if slot has value
bool loadWiFiSlot(int slot)
{
    String keySsid = slot == 1 ? "ssid1" : "ssid2";
    String keyPass = slot == 1 ? "pass1" : "pass2";
    ssid = prefGetString(keySsid, "");
    pass = prefGetString(keyPass, "");
    return ssid.length() > 0;
}

// Sort 2 networks by signal (if both present)
void sortWiFiBySignal()
{
    String s1 = prefGetString("ssid1", "");
    String p1 = prefGetString("pass1", "");
    String s2 = prefGetString("ssid2", "");
    String p2 = prefGetString("pass2", "");

    if (s1 == "" || s2 == "")
        return;

    WiFi.scanNetworks();
    int r1 = getRSSIOf(s1);
    int r2 = getRSSIOf(s2);

    if (r2 > r1)
    {
        prefPutString("ssid1", s2);
        prefPutString("pass1", p2);
        prefPutString("ssid2", s1);
        prefPutString("pass2", p1);
        Serial.println("Sorted WiFi by RSSI (slot2 stronger → swapped).");
    }
}

// Save WiFi → shift slot1 → slot2 and put new into slot1
void saveWiFi(const String &S, const String &P)
{
    String oldS = prefGetString("ssid1", "");
    String oldP = prefGetString("pass1", "");
    prefPutString("ssid2", oldS);
    prefPutString("pass2", oldP);
    prefPutString("ssid1", S);
    prefPutString("pass1", P);
    Serial.printf("Saved WiFi '%s' -> slot1\n", S.c_str());
}

// Reset all WiFi
void resetWiFi()
{
    prefClearAll();
    Serial.println("WiFi credentials RESET.");
}

// ================= State-machine connect system =================

void startConnectSlot(int slot)
{
    if (!loadWiFiSlot(slot))
    {
        wifiState = WIFI_FAILED;
        return;
    }

    Serial.printf("Attempt connect to [%s] (slot %d)\n", ssid.c_str(), slot);

    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);

    if (useStaticIP)
    {
        WiFi.config(static_IP, static_gateway, static_subnet);
    }

    WiFi.begin(ssid.c_str(), pass.c_str()); // non-blocking in our state machine

    wifiState = WIFI_CONNECTING;
    wifiRetryTimer = millis();
}

void startAccessPointMode(); // forward

void handleWifi()
{
    // called from loop()
    if (wifiState == WIFI_IDLE)
        return;

    if (wifiState == WIFI_CONNECTING)
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            wifiState = WIFI_CONNECTED;
            ipAddress = WiFi.localIP().toString();
            Serial.printf("Connected to %s, IP: %s\n", ssid.c_str(), ipAddress.c_str());
            onWifiConnected();
            return;
        }

        // Timeout for connection attempt
        if (millis() - wifiRetryTimer > 10000)
        {
            Serial.println("Connect timeout -> next slot");
            currentSlot++;
            if (currentSlot > 2)
            {
                wifiState = WIFI_FAILED;
            }
            else
            {
                startConnectSlot(currentSlot);
            }
        }
    }

    else if (wifiState == WIFI_CONNECTED)
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("WiFi disconnected -> will retry later");
            wifiState = WIFI_IDLE;
            wifiRetryTimer = millis();
        }
    }

    else if (wifiState == WIFI_FAILED)
    {
        Serial.println("All stored WiFi failed -> starting AP mode");
        startAccessPointMode();
        wifiState = WIFI_IDLE;
    }

    // Reconnect trigger when idle and timer expired
    if (wifiState == WIFI_IDLE)
    {
        // if device previously had been connected and lost connection, allow periodic reconnect attempts
        static unsigned long lastRetryCheck = 0;
        if (millis() - lastRetryCheck > WIFI_RETRY_INTERVAL)
        {
            lastRetryCheck = millis();
            // try connect if stored networks available
            if (loadWiFiSlot(1) || loadWiFiSlot(2))
            {
                currentSlot = 1;
                startConnectSlot(currentSlot);
            }
        }
    }
}

// Kick-off connection sequence
void connectWifi()
{
    Serial.println("connectWifi(): sorting by signal and attempting connect");
    sortWiFiBySignal();
    currentSlot = 1;
    startConnectSlot(currentSlot);
}

// ================= Web UI HTML =================

const char *WIFI_HTML = R"=====(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WiFi Configuration</title>
<style>
    body { font-family: Arial, sans-serif; background: #efefef; margin: 0; padding: 20px; }
    .container { max-width: 600px; background: white; margin: auto; padding: 25px; border-radius: 12px; box-shadow: 0 3px 10px rgba(0,0,0,0.1); }
    h1 { margin-top: 0; }
    label { font-weight: bold; display:block; margin-top:10px; }
    input[type=text], input[type=password] { width:100%; box-sizing:border-box; padding:14px; margin-top:8px; margin-bottom:14px; border-radius:6px; border:1px solid #aaa; font-size:16px; }
    button { width:100%; padding:14px; font-size:18px; background:#4CAF50; color:#fff; border:none; border-radius:6px; cursor:pointer; }
    button.secondary { background:#eee; color:#000; margin-top:8px; }
    .wifi-item { padding:12px; margin-bottom:8px; background:#f5f5f5; border-radius:6px; border:1px solid #ddd; cursor:pointer; }
</style>
</head>
<body>
<div class="container">
  <h1>WiFi Configuration</h1>
  <label>WiFi SSID</label>
  <input id="ssid" type="text" placeholder="Enter WiFi name">
  <label>Password</label>
  <input id="pass" type="password" placeholder="Password">
  <button onclick="saveWifi()">Save & Connect</button>
  <button class="secondary" onclick="resetWifi()">Reset Saved WiFi</button>

  <h2>Nearby WiFi</h2>
  <div id="list">Loading...</div>
</div>

<script>
function renderWifiList(list) {
  const box = document.getElementById("list");
  box.innerHTML = "";
  list.sort((a,b)=>b.rssi - a.rssi);
  list.forEach(item=>{
    const d = document.createElement("div");
    d.className = "wifi-item";
    d.textContent = item.ssid + " (" + item.rssi + " dBm)";
    d.onclick = ()=> document.getElementById("ssid").value = item.ssid;
    box.appendChild(d);
  });
}

function loadWifi() {
  fetch("/scan")
    .then(r => r.json())
    .then(d => renderWifiList(d))
    .catch(()=> document.getElementById("list").innerText = "Failed to load");
}

function saveWifi() {
  const ssid = document.getElementById("ssid").value;
  const pass = document.getElementById("pass").value;
  if(!ssid) { alert("SSID required"); return; }
  fetch("/save?ssid="+encodeURIComponent(ssid)+"&pass="+encodeURIComponent(pass))
    .then(()=> { alert("Saved! Device will attempt to connect."); })
    .catch(()=> alert("Save failed"));
}

function resetWifi() {
  if(!confirm("Reset all saved WiFi?")) return;
  fetch("/reset")
    .then(()=> alert("Reset done"))
    .catch(()=> alert("Reset failed"));
}

window.onload = function() { loadWifi(); setInterval(loadWifi, 8000); };
</script>
</body>
</html>
)=====";

// ================= Web server handlers =================

String URLDecode(const String &str);

void handleRoot()
{
    wifiServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    wifiServer.sendHeader("Pragma", "no-cache");
    wifiServer.sendHeader("Expires", "0");
    wifiServer.send(200, "text/html", WIFI_HTML);
}

void handleScan()
{
    // do a fresh scan and return JSON
    String out = scanWiFi();
    wifiServer.send(200, "application/json", out);
}

void handleSave()
{
    // Accept via query params: ?ssid=...&pass=...
    String s = "";
    String p = "";
    if (wifiServer.method() == HTTP_POST)
    {
        // try to parse body as form or plain - but UI uses GET query
        if (wifiServer.hasArg("plain"))
        {
            String body = wifiServer.arg("plain");
            // naive parse: look for ssid=...&pass=...
            int i1 = body.indexOf("ssid=");
            int i2 = body.indexOf("&pass=");
            if (i1 >= 0)
            {
                if (i2 > i1)
                    s = URLDecode(body.substring(i1 + 5, i2));
                int i3 = body.indexOf("pass=");
                if (i3 >= 0)
                    p = URLDecode(body.substring(i3 + 5));
            }
        }
    }
    if (s.length() == 0)
    {
        if (wifiServer.hasArg("ssid"))
            s = wifiServer.arg("ssid");
        if (wifiServer.hasArg("pass"))
            p = wifiServer.arg("pass");
    }

    if (s.length() == 0)
    {
        wifiServer.send(400, "text/plain", "ssid required");
        return;
    }

    saveWiFi(s, p);
    wifiServer.send(200, "text/plain", "OK");

    // attempt to connect soon
    delay(200);
    connectWifi();
}

// simple URL decode helper for POST body parsing fallback
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
            String hex = str.substring(i + 1, i + 3);
            char val = (char)strtol(hex.c_str(), nullptr, 16);
            ret += val;
            i += 2;
        }
        else
            ret += c;
    }
    return ret;
}

void handleReset()
{
    resetWiFi();
    wifiServer.send(200, "text/plain", "RESET");
}

// ================= AP Mode start (includes starting webserver) =================

void startAccessPointMode()
{
    Serial.println("Starting AP mode (open)...");
    WiFi.disconnect();
    delay(200);
    WiFi.mode(WIFI_AP);
    delay(200);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    delay(200);
    WiFi.softAP(apSsid.c_str()); // open AP (no password)
    if (!serverStarted)
    {
        wifiServer.begin();
        serverStarted = true;
        Serial.println("Web server started on 192.168.4.1");
    }
}

// ================= INIT / LOOP helpers to call from sketch =================

// Begin wifi manager (call from setup)
void wifiManagerBegin()
{
    Serial.println("wifiManagerBegin()");
#if defined(ESP8266)
    if (!LittleFS.begin())
    {
        LittleFS.format();
        File f = LittleFS.open("/root.txt", "w");
        if (f)
        {
            f.println("");
            f.close();
            Serial.println("LittleFS mounted");
        }
        else
        {
            Serial.println("LittleFS.begin() failed - make sure LittleFS is available");
        }
    }
    else
    {
        Serial.println("LittleFS mounted");
    }
#endif

    // get MAC if needed
    getMacAddress();

    // register server handlers now (they will be active when AP starts)
    registerServerRoutes();

    // Do not call server.begin() here; call when AP starts.
    serverStarted = false;

    // If stored networks exist, try to connect automatically at boot
    bool has1 = false, has2 = false;
#if defined(ESP8266)
    preferences.begin("wifi", true);
    has1 = preferences.getString("ssid1", "").length() > 0;
    has2 = preferences.getString("ssid2", "").length() > 0;
    preferences.end();
#elif defined(ESP32)
    preferences.begin("wifi", true);
    has1 = preferences.getString("ssid1", "").length() > 0;
    has2 = preferences.getString("ssid2", "").length() > 0;
    preferences.end();
#endif

    if (has1 || has2)
    {
        // prefer stronger
        sortWiFiBySignal();
        connectWifi();
    }
    else
    {
        // no saved WiFi -> start AP immediately
        startAccessPointMode();
    }
}

// Must be called in the sketch loop()
void wifiManagerLoop()
{
    // handle web server clients if started
    if (serverStarted)
    {
        wifiServer.handleClient();
    }
    // handle wifi state machine
    handleWifi();
}