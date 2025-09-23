/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2025 Moniruzzaman Akash
 * moniruzzaman.akash@unh.edu
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 */

// ESP32C3 used: https://a.co/d/elhNUnP



#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>
#include <time.h>
#include <esp_system.h>   // for esp_read_mac
#include <esp_wifi.h>     // (optional on some cores)

#include <unordered_map>
#include <vector>
#include <string>
#include <cctype>

// ===================== Debug toggles =====================
// #define DEBUG_NET   1
// #define DEBUG_MQTT  1

// ===================== Hardware / Behavior =====================
static constexpr int LED_PIN         = 12;    // adjust to your board
static constexpr int AP_TRIGGER_PIN  = 2;     // hold LOW to enter AP
static constexpr uint32_t AP_HOLD_MS = 1000;  // 1s press
static time_t ts_unix_last_sensor_update = 0; // last unix timestamp when sensor data was sent

// Admin token: used ONLY to authorize saves; never stored in NVS
static const char* ADMIN_TOKEN = "123456";

// ===================== Config & Globals =====================
struct Config {
  char ssid[32]       = "ssid";
  char pass[64]       = "pass";
  char mqttHost[64]   = "192.168.50.237";
  uint16_t mqttPort   = 1883;
  char deviceID[32]   = "BS1";
  char macList[160]   = "dd:88:00:00:13:07"; // lower-case, comma-separated
  uint16_t pubMs      = 100;                 // min publish interval per beacon
};

Preferences prefs;
Config cfg;
std::string chipId;
WebServer http(80);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

struct BeaconState { float rssi_ema = NAN; int lastRSSI = 0; uint32_t lastPubMs = 0; };
std::unordered_map<std::string, BeaconState> states;
std::string mac; // currently detected MAC in scan callback
std::string mac_filtered; // currently detected MAC after filtering

static bool g_inAPMode = false;
static volatile bool g_timeReady = false;

// ===================== Helpers =====================
static inline const char* showStr(const char* s) { return (s && *s) ? s : "(empty)"; }

// Pretty-print current config to Serial
static void printConfig() {
  Serial.println(F("=== NVS Config (namespace: ble-cfg) ==="));
  Serial.printf("ssid:        %s\n", showStr(cfg.ssid));
  Serial.printf("pass:        %s\n", showStr(cfg.pass)); //(*cfg.pass) ? "********" : "(empty)");
  Serial.printf("mqttHost:    %s\n", showStr(cfg.mqttHost));
  Serial.printf("mqttPort:    %u\n", cfg.mqttPort);
  Serial.printf("deviceID:    %s\n", showStr(cfg.deviceID));
  Serial.printf("macList:     %s\n", showStr(cfg.macList));
  Serial.printf("pubMs:       %u\n", cfg.pubMs);
  Serial.println(F("======================================="));
}

// Load config from NVS; auto-create namespace if missing; print values
static void loadConfig(bool verbose = true) {
  // Try RO open; if missing, create RW once then reopen RO
  if (!prefs.begin("ble-cfg", true)) {
    prefs.end();
    if (prefs.begin("ble-cfg", false)) {
      prefs.end();
      prefs.begin("ble-cfg", true);
    } else {
      if (verbose) Serial.println(F("[NVS] Failed to open/create namespace (ble-cfg)"));
      return;
    }
  }

  size_t need = prefs.getBytesLength("json");
  if (need > 0 && need < 4096) {
    std::unique_ptr<char[]> buf(new char[need + 1]);
    prefs.getBytes("json", buf.get(), need);
    buf[need] = 0;

    DynamicJsonDocument d(4096);
    if (deserializeJson(d, buf.get()) == DeserializationError::Ok) {
      strlcpy(cfg.ssid,       d["ssid"]       | cfg.ssid,       sizeof(cfg.ssid));
      strlcpy(cfg.pass,       d["pass"]       | cfg.pass,       sizeof(cfg.pass));
      strlcpy(cfg.mqttHost,   d["mqttHost"]   | cfg.mqttHost,   sizeof(cfg.mqttHost));
      cfg.mqttPort =           d["mqttPort"]  | cfg.mqttPort;
      strlcpy(cfg.deviceID, d["deviceID"] | cfg.deviceID, sizeof(cfg.deviceID));
      strlcpy(cfg.macList,    d["macList"]    | cfg.macList,    sizeof(cfg.macList));
      cfg.pubMs =              d["pubMs"]     | cfg.pubMs;
    } else if (verbose) {
      Serial.println(F("[NVS] JSON parse error, using defaults"));
    }
  } else if (verbose) {
    Serial.println(F("[NVS] No existing config, using defaults"));
  }

  prefs.end();
  if (verbose) printConfig();
}

// Save selected fields to NVS (no token ever)
static void saveConfigFromJson(const JsonVariantConst& d) {
  DynamicJsonDocument out(4096);
  out["ssid"]       = d["ssid"]       | cfg.ssid;
  out["pass"]       = d["pass"]       | cfg.pass;
  out["mqttHost"]   = d["mqttHost"]   | cfg.mqttHost;
  out["mqttPort"]   = d["mqttPort"]   | cfg.mqttPort;
  out["deviceID"]   = d["deviceID"]   | cfg.deviceID;
  out["macList"]    = d["macList"]    | cfg.macList;
  out["pubMs"]      = d["pubMs"]      | cfg.pubMs;

  String s; serializeJson(out, s);
  if (!prefs.begin("ble-cfg", false)) {
    Serial.println(F("[NVS] Failed to open ble-cfg for write"));
    return;
  }
  prefs.putBytes("json", s.c_str(), s.length());
  prefs.end();

  // Reflect into RAM immediately
  strlcpy(cfg.ssid,       out["ssid"],       sizeof(cfg.ssid));
  strlcpy(cfg.pass,       out["pass"],       sizeof(cfg.pass));
  strlcpy(cfg.mqttHost,   out["mqttHost"],   sizeof(cfg.mqttHost));
  cfg.mqttPort =          out["mqttPort"];
  strlcpy(cfg.deviceID, out["deviceID"], sizeof(cfg.deviceID));
  strlcpy(cfg.macList,    out["macList"],    sizeof(cfg.macList));
  cfg.pubMs =             out["pubMs"];

  Serial.println(F("[NVS] Saved config:"));
  printConfig();
}

// ===================== SNTP time =====================
static void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com"); // keep UTC
}
static time_t nowUnix() {
  time_t t = time(nullptr);
  if (t > 1600000000) g_timeReady = true;
  return t;
}

// ===================== LED state machine =====================
enum class LedMode { OFF, AP_SOLID, CONNECTING_FAST, ONLINE_HEARTBEAT };
static LedMode ledMode = LedMode::OFF;
static uint32_t ledTickMs = 0;
static bool ledLevel = LOW;

static void ledSetMode(LedMode m) {
  ledMode = m; ledTickMs = millis();
  switch (m) {
    case LedMode::AP_SOLID: ledLevel = HIGH; break;
    case LedMode::OFF:
    case LedMode::CONNECTING_FAST:
    case LedMode::ONLINE_HEARTBEAT: default: ledLevel = LOW; break;
  }
  digitalWrite(LED_PIN, ledLevel);
}
static void ledUpdate() {
  const uint32_t t = millis();
  switch (ledMode) {
    case LedMode::AP_SOLID: break; // solid ON
    case LedMode::CONNECTING_FAST:
      if (t - ledTickMs >= 250) { ledTickMs = t; ledLevel = !ledLevel; digitalWrite(LED_PIN, ledLevel); }
      break;
    case LedMode::ONLINE_HEARTBEAT:
      if (t - ledTickMs >= 10000) { digitalWrite(LED_PIN, HIGH); delay(80); digitalWrite(LED_PIN, LOW); ledTickMs = t; }
      break;
    case LedMode::OFF:
    default: digitalWrite(LED_PIN, LOW); break;
  }
}

// ===================== Wi-Fi =====================
static bool wifiConnect(uint32_t timeoutMs = 15000) {
  ledSetMode(LedMode::CONNECTING_FAST);
#if DEBUG_NET
  Serial.printf("[WiFi] Connecting to SSID='%s', Pass: '%s'\n", cfg.ssid, cfg.pass);
#endif
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  uint32_t t0 = millis(), lastDot = 0;
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) {
    ledUpdate();
    if (millis() - lastDot > 500) { lastDot = millis(); Serial.print("."); }
    delay(20);
  }
  bool ok = WiFi.status() == WL_CONNECTED;
#if DEBUG_NET
  Serial.println();
  if (ok) {
    Serial.printf("[WiFi] OK. IP=%s  RSSI=%d dBm\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("[WiFi] FAILED");
  }
#endif
  ledSetMode(ok ? LedMode::ONLINE_HEARTBEAT : LedMode::OFF);
  return ok;
}

// ===================== Web UI =====================
static void sendConfigForm(bool inAP) {
  String ip = inAP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String title = String("BLE Beacon Tracker Sensor Setup (") + (inAP ? "AP" : "STA") + ") - " + String(chipId.c_str());

  String html;
  html.reserve(3000);
  html += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>BLE Sensor Config</title><style>"
            "body{font-family:system-ui,Arial,sans-serif;margin:16px;background:#0b0e14;color:#e6e6e6}"
            ".card{max-width:780px;margin:auto;background:#141a23;border:1px solid #223042;border-radius:12px;padding:16px}"
            "label{display:block;margin-top:10px;color:#98a2b3}input{width:100%;padding:8px;border-radius:8px;border:1px solid #223042;background:#0b0e14;color:#e6e6e6}"
            ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}.row>div{min-width:0}"
            ".btn{margin-top:14px;padding:10px 14px;border:0;border-radius:10px;background:#3b82f6;color:#fff;cursor:pointer}"
            ".muted{color:#98a2b3;font-size:12px;margin-top:6px}</style></head><body><div class='card'>");
  html += "<h3>" + title + "</h3>";
  html += "<div class='muted'>Device IP: " + ip + " · Host: " + String(cfg.deviceID) + ".local</div>";
  html += F("<form method='POST' action='/form'>"
            "<label>Admin token (required to save)</label><input name='token' type='password' placeholder='required'>"
            "<div class='row'><div>"
            "<label>Wi-Fi SSID</label><input name='ssid' value='"); html += cfg.ssid; html += F("'></div><div>"
            "<label>Wi-Fi Password</label><input name='pass' type='password' value='"); html += cfg.pass; html += F("'></div></div>"
            "<div class='row'><div>"
            "<label>MQTT Host</label><input name='mqttHost' value='"); html += cfg.mqttHost; html += F("'></div><div>"
            "<label>MQTT Port</label><input name='mqttPort' type='number' value='"); html += String(cfg.mqttPort); html += F("'></div></div>"
            "<div class='row'><div>"
            "<label>Device ID</label><input name='deviceID' value='"); html += cfg.deviceID; html += F("'></div><div>"
            "<label>Publish Min Interval (ms)</label><input name='pubMs' type='number' value='"); html += String(cfg.pubMs); html += F("'></div></div>"
            "<label>Tracked MACs (comma separated, lowercase)</label>"
            "<input name='macList' value='"); html += cfg.macList; html += F("'>"
            "<div class='muted'>Example: dd:88:00:00:13:07, a1:b2:c3:d4:e5:f6</div>"
            "<button class='btn' type='submit'>Save & Reboot</button></form>"
            "<div class='muted' style='margin-top:10px'>Status: <code>/status</code> · JSON Config: <code>/config</code></div>"
            "</div></body></html>");

  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, "text/html", "");
  http.sendContent(html);
}

static void handleFormPost() {
  String token = http.arg("token");
  if (!token.length() || token != ADMIN_TOKEN) {
    http.send(403, "text/plain", "Forbidden: bad token");
    return;
  }
  DynamicJsonDocument d(4096);
  d["ssid"]       = http.arg("ssid");
  d["pass"]       = http.arg("pass");
  d["mqttHost"]   = http.arg("mqttHost");
  d["mqttPort"]   = http.arg("mqttPort").toInt();
  d["deviceID"]   = http.arg("deviceID");
  d["macList"]    = http.arg("macList");
  d["pubMs"]      = http.arg("pubMs").toInt();
  // no token saved

  saveConfigFromJson(d.as<JsonVariantConst>());

  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, "text/html", "");
  http.sendContent(F("<!doctype html><meta charset='utf-8'><title>Saved</title>"
                     "<body style='font-family:system-ui;'><h3>Saved. Rebooting…</h3>"
                     "<p>If it doesn’t reconnect, join the new Wi-Fi and visit <code>/status</code>.</p></body>"));
  http.client().flush();
  delay(500);
  ESP.restart();
}

// ===================== AP / STA servers =====================
static void startAPForProvision() {
  loadConfig(true); // ensure form is prefilled + printed to Serial
  String ap = String("C3-Setup-") + String(chipId.c_str());
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap.c_str());
  g_inAPMode = true;
  ledSetMode(LedMode::AP_SOLID);

  http.on("/", HTTP_GET, [](){ sendConfigForm(true); });
  http.on("/form", HTTP_POST, handleFormPost);
  http.on("/status", HTTP_GET, [](){
    DynamicJsonDocument s(1024);
    s["chip"] = chipId.c_str(); s["mode"] = "AP"; s["ip"] = WiFi.softAPIP().toString();
    String body; serializeJson(s, body); http.send(200, "application/json", body);
  });
  // JSON config endpoint (token required; never persisted)
  http.on("/config", HTTP_POST, [](){
    DynamicJsonDocument d(4096);
    DeserializationError e = deserializeJson(d, http.arg("plain"));
    if (e) { http.send(400, "text/plain", "bad json"); return; }
    if (strcmp(d["token"] | "", ADMIN_TOKEN) != 0) { http.send(403, "text/plain", "bad token"); return; }
    d.remove("token"); // never store
    saveConfigFromJson(d);
    http.send(200, "text/plain", "saved, rebooting"); delay(500); ESP.restart();
  });

  http.begin();
}

static void startHttpSTA() {
  http.on("/", HTTP_GET, [](){ sendConfigForm(false); });
  http.on("/form", HTTP_POST, handleFormPost);
  http.on("/status", HTTP_GET, [](){
    DynamicJsonDocument s(1024);
    s["chip"]=chipId.c_str(); s["mode"]="STA"; s["ip"]=WiFi.localIP().toString();
    s["ssid"]=cfg.ssid; s["mqttHost"]=cfg.mqttHost; s["mqttPort"]=cfg.mqttPort;
    String body; serializeJson(s, body); http.send(200, "application/json", body);
  });
  http.on("/config", HTTP_POST, [](){
    DynamicJsonDocument d(4096);
    DeserializationError e = deserializeJson(d, http.arg("plain"));
    if (e) { http.send(400, "text/plain", "bad json"); return; }
    if (strcmp(d["token"] | "", ADMIN_TOKEN) != 0) { http.send(403, "text/plain", "bad token"); return; }
    d.remove("token");
    saveConfigFromJson(d);
    http.send(200, "text/plain", "saved, rebooting"); delay(500); ESP.restart();
  });
  http.begin();
}

static void enterAPModeNow() {
  NimBLEScan* sc = NimBLEDevice::getScan();
  if (sc) sc->stop();
  if (mqtt.connected()) mqtt.disconnect();
  WiFi.disconnect(true, true);
  delay(100);
  startAPForProvision();
}

// ===================== mDNS =====================
static void startMDNS() {
  String host = String(cfg.deviceID);
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("_ble-rssi", "_tcp", 80);
    MDNS.addServiceTxt("_ble-rssi", "_tcp", "id", chipId.c_str());
  }
}

// ===================== MQTT =====================

unsigned long g_nextMqttRetryMs = 0;
uint8_t g_mqttRetries = 0;

const char* mqttStateStr(int s){
  switch(s){
    case MQTT_CONNECTION_TIMEOUT:      return "CONNECTION_TIMEOUT";
    case MQTT_CONNECTION_LOST:         return "CONNECTION_LOST";
    case MQTT_CONNECT_FAILED:          return "CONNECT_FAILED";
    case MQTT_DISCONNECTED:            return "DISCONNECTED";
    case MQTT_CONNECTED:               return "CONNECTED";
    case MQTT_CONNECT_BAD_PROTOCOL:    return "BAD_PROTOCOL";
    case MQTT_CONNECT_BAD_CLIENT_ID:   return "BAD_CLIENT_ID";
    case MQTT_CONNECT_UNAVAILABLE:     return "SERVER_UNAVAILABLE";
    case MQTT_CONNECT_BAD_CREDENTIALS: return "BAD_CREDENTIALS";
    case MQTT_CONNECT_UNAUTHORIZED:    return "UNAUTHORIZED";
    default:                           return "UNKNOWN";
  }
}

void mqttConnectRobust() {
  if (WiFi.status() != WL_CONNECTED) return;

  const unsigned long now = millis();
  if (now < g_nextMqttRetryMs) return;   // wait until next backoff slot

  mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  mqtt.setKeepAlive(30);
  mqtt.setBufferSize(512);
  std::string clientId  = std::string("ble-") + cfg.deviceID;
  std::string willTopic = std::string("sensors/ble/") + cfg.deviceID + "/status";

  // Close any half-open TCP before new attempt
  wifiClient.stop();

  Serial.printf("[MQTT] Connecting to %s:%u as %s\n", cfg.mqttHost, cfg.mqttPort, clientId.c_str());

  bool ok = mqtt.connect(clientId.c_str(),
                         /*willTopic*/ willTopic.c_str(), /*willQos*/ 0, /*willRetain*/ true,
                         /*willMessage*/ "offline");

  if (!ok) {
    int st = mqtt.state();
    Serial.printf("[MQTT] connect failed: state=%d (%s)\n", st, mqttStateStr(st));
    // Backoff: 0.5s,1s,2s,... up to 60s
    g_mqttRetries = (g_mqttRetries < 8) ? g_mqttRetries + 1 : 8;
    unsigned long delayMs = 500UL * (1UL << (g_mqttRetries - 1));
    if (delayMs > 60000UL) delayMs = 60000UL;
    g_nextMqttRetryMs = now + delayMs;
    return;
  }

  // Success
  g_mqttRetries = 0;
  g_nextMqttRetryMs = 0;
  mqtt.publish(willTopic.c_str(), "online", true);
  Serial.println("[MQTT] connected.");
}


// ===================== BLE scanning =====================
static std::vector<std::string> targetMacs;

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* adv) override {
    mac = adv->getAddress().toString();
    for (char &c : mac) c = (char)std::tolower((unsigned char)c);

    bool match = false;
    for (auto& m : targetMacs) { if (mac == m) { match = true; break; } }
    if (!match) return;

    mac_filtered = mac;

    int rssi = adv->getRSSI();
    auto &st = states[mac];
    if (isnan(st.rssi_ema)) st.rssi_ema = rssi; else st.rssi_ema = 0.3f * rssi + 0.7f * st.rssi_ema;

    ts_unix_last_sensor_update = nowUnix();

    uint32_t t = millis();
    if (t - st.lastPubMs < cfg.pubMs) return;

    // JSON (no distance)
    StaticJsonDocument<256> d;
    d["sensor_mac"]  = chipId.c_str();
    d["sensor_id"]  = cfg.deviceID;
    d["beacon_mac"]     = mac.c_str();
    d["rssi"]    = rssi;
    d["rssi_ema"]     = st.rssi_ema;
    d["ts_unix"] = (uint32_t) nowUnix(); // UTC seconds
    d["ts_ms"]   = (uint32_t) millis();  // uptime ms
    d["ip"]      = WiFi.localIP().toString();

    char buf[256]; size_t n = serializeJson(d, buf);
    std::string topic = std::string("sensors/ble/");

#if DEBUG_MQTT
    Serial.print("[MQTT] ");
    Serial.print(topic.c_str());
    Serial.print(" ");
    Serial.write(buf, n);
    Serial.println();
#endif

    bool pubOK = mqtt.publish(topic.c_str(), (const uint8_t*)buf, (unsigned int)n);
    if (!pubOK) {
      Serial.println("[MQTT] publish failed; scheduling reconnect");
      mqtt.disconnect();
      g_nextMqttRetryMs = 0;  // allow immediate retry
    }

    st.lastPubMs = t;
  }
};
static ScanCB scanCb;

static void startBLE() {
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(false); // setting true will send a request to broadcaster
  scan->setInterval(100);
  scan->setWindow(40);
  scan->setMaxResults(0); // don't store results in RAM, callbacks only
  scan->setDuplicateFilter(false);
  scan->setScanCallbacks(&scanCb, /*wantDuplicates=*/true);
  scan->start(0, false, false);  // forever
}

// ===================== Setup & Loop =====================
void setup() {
    Serial.begin(115200);
    unsigned long t = millis();
    while (!Serial && millis() - t < 3000) { delay(10); }

    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
    pinMode(AP_TRIGGER_PIN, INPUT_PULLUP);
    delay(50);

    // Early AP long-press
    uint32_t t0 = millis(); bool stayedLow = true;
    while (millis() - t0 < AP_HOLD_MS) { if (digitalRead(AP_TRIGGER_PIN) != LOW) { stayedLow = false; break; } delay(10); }

    // Build deviceId from full 48-bit STA MAC (unique)
    uint8_t mac_esp[6];
    esp_read_mac(mac_esp, ESP_MAC_WIFI_STA);  // STA MAC straight from eFuse
    Serial.printf("[BOOT] MCU_MAC = %02X:%02X:%02X:%02X:%02X:%02X\n", mac_esp[0], mac_esp[1], mac_esp[2], mac_esp[3], mac_esp[4], mac_esp[5]);
    
    char idbuf[13]; // 12 hex + NUL
    snprintf(idbuf, sizeof(idbuf), "%02X%02X%02X%02X%02X%02X", mac_esp[0], mac_esp[1], mac_esp[2], mac_esp[3], mac_esp[4], mac_esp[5]);
    chipId = idbuf;
    
    loadConfig(true);  // prints NVS content

    // If Wi-Fi not configured (factory default), go straight to AP mode
    if (strcmp(cfg.ssid, "ssid") == 0 || cfg.ssid[0] == '\0') {
        Serial.println("[BOOT] No Wi-Fi configured → entering AP provisioning");
        enterAPModeNow();
        return;
    }


    // Parse MAC list into vector<string>
    targetMacs.clear();
    { String s = cfg.macList; s.toLowerCase(); s += ","; int p = 0;
        while (true) { int q = s.indexOf(',', p); if (q < 0) break;
        String m = s.substring(p, q); m.trim(); if (m.length() == 17) targetMacs.emplace_back(m.c_str()); p = q + 1; } }

    if (stayedLow) { Serial.println("AP trigger at boot → AP mode"); enterAPModeNow(); return; }

    if (!wifiConnect(15000)) {
        Serial.println("Wi-Fi join failed → Rebooting");
        delay(500); ESP.restart();
        return;
    }

    Serial.printf("Wi-Fi OK, IP=%s\n", WiFi.localIP().toString().c_str());
    setupTime();
    // Start STA HTTP routes
    http.on("/", HTTP_GET, [](){ sendConfigForm(false); });
    http.on("/form", HTTP_POST, handleFormPost);
    http.on("/status", HTTP_GET, [](){
        DynamicJsonDocument s(1024);
        s["chip"]=chipId.c_str(); s["mode"]="STA"; s["ip"]=WiFi.localIP().toString();
        s["ssid"]=cfg.ssid; s["mqttHost"]=cfg.mqttHost; s["mqttPort"]=cfg.mqttPort;
        s["beacon_mac"] = mac_filtered.c_str(); s["rssi_ema"] = states[mac_filtered].rssi_ema;
        s["ts_unix"] = (uint32_t) ts_unix_last_sensor_update; s["ts_ms"] = (uint32_t) millis();
        s["state"]      = mqttStateStr(mqtt.state());   // readable string
        s["retries"]    = g_mqttRetries;
        s["next_retry_ms"] = (millis() > g_nextMqttRetryMs) ? 0 : (g_nextMqttRetryMs - millis());
        String body; serializeJson(s, body); http.send(200, "application/json", body);
    });
    http.on("/config", HTTP_POST, [](){
        DynamicJsonDocument d(4096);
        DeserializationError e = deserializeJson(d, http.arg("plain"));
        if (e) { http.send(400, "text/plain", "bad json"); return; }
        if (strcmp(d["token"] | "", ADMIN_TOKEN) != 0) { http.send(403, "text/plain", "bad token"); return; }
        d.remove("token");
        saveConfigFromJson(d);
        http.send(200, "text/plain", "saved, rebooting"); delay(500); ESP.restart();
    });
    http.begin();

    // mDNS + MQTT + BLE
    // if (MDNS.begin(cfg.deviceID)) {
    //     MDNS.addService("_ble-rssi", "_tcp", 80);
    //     MDNS.addServiceTxt("_ble-rssi", "_tcp", "id", chipId.c_str());
    // }
    startMDNS();
    startBLE();
    mqttConnectRobust();
}

void loop() {
  // Runtime AP long-press
  static uint32_t lowSince = 0;
  if (!g_inAPMode) {
    int level = digitalRead(AP_TRIGGER_PIN);
    if (level == LOW) {
      if (lowSince == 0) lowSince = millis();
      if (millis() - lowSince >= AP_HOLD_MS) { Serial.println("AP trigger at runtime → AP mode"); enterAPModeNow(); }
    } else lowSince = 0;
  }

  http.handleClient();
  if (WiFi.isConnected()) {
    if (!mqtt.connected()) mqttConnectRobust();
    mqtt.loop();
  }

  ledUpdate();
  if (!g_timeReady) nowUnix(); // wait for SNTP
  delay(5);
}
