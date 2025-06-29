/*
  src/main.cpp - Door Controller Firmware (ESP8266/D1 Mini)
  - Versjon: 1.0.0
  - Wiegand (26, 34, 36, AUTO)
  - Edge/rule engine (HTTP, MQTT, GPIO, logg)
  - Web UI (settings, dashboard, access, rules, log)
  - Brukerhåndtering (users/Wiegand tags)
  - MQTT: open, lock, add/delete user
  - OTA, loggbuffer, 4-sifret adminkode
  - Reset/fabrikkreset med knapp (D3)
  - MIT License
*/

#define FW_VERSION "1.0.0"

#include <Arduino.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <Wiegand.h>
#include <deque>


// ------------------- SETTINGS ----------------------
#define ADMIN_CODE "9792"
#define LOG_SIZE 50
#define RELAY_PIN D1
#define ALBUE_PIN D7
#define DOOR_PIN D2
#define WIEGAND_D0 D5
#define WIEGAND_D1 D6
#define ONBOARD_LED D4
#define RESET_PIN D3 // FLASH-knappen

AsyncWebServer server(80);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WIEGAND wg;

struct AccessUser { String username; String tag; };
struct Rule { String name; String trigger; String param; String action; String actparam; };
std::vector<AccessUser> accessUsers;
std::vector<Rule> rules;
std::deque<String> logbuf;
String device_id, ssid, password, wiegand_mode, mqtt_host, mqtt_user, mqtt_pass, admin_code = ADMIN_CODE, ota_url;
int mqtt_port = 1883, relay_ms = 500;

// ---- LOGGING ----
void addLog(String s) {
  String t = "[" + String(millis()/1000) + "s] " + s;
  logbuf.push_back(t); if (logbuf.size() > LOG_SIZE) logbuf.pop_front();
  Serial.println(t);
}

// ---- RESET/FABRIKKRESET KNAPP ----
void handleButtonReset() {
  static uint32_t pressedStart = 0;
  static bool prevState = true;
  bool nowState = digitalRead(RESET_PIN) == HIGH;
  if (!nowState && prevState) pressedStart = millis();
  if (!nowState && (millis() - pressedStart > 29500)) { // >30 sek
    addLog("Factory reset (settings slettet)");
    LittleFS.remove("/settings.json");
    delay(200); ESP.restart();
  } else if (!nowState && (millis() - pressedStart > 4500)) { // >5 sek
    addLog("Manual reboot");
    ESP.restart();
  }
  if (nowState) pressedStart = millis();
  prevState = nowState;
}

// ---- RELAY ----
void triggerRelay(int pulseMs = -1) {
  digitalWrite(RELAY_PIN, HIGH); addLog("Relay ON");
  delay(pulseMs > 0 ? pulseMs : relay_ms);
  digitalWrite(RELAY_PIN, LOW); addLog("Relay OFF");
}

// ---- SETTINGS ----
void saveSettings() {
  DynamicJsonDocument doc(4096);
  doc["fw_version"] = FW_VERSION;
  doc["device_id"] = device_id; doc["ssid"] = ssid; doc["password"] = password;
  doc["relay_ms"] = relay_ms; doc["wiegand_mode"] = wiegand_mode;
  doc["mqtt_host"] = mqtt_host; doc["mqtt_port"] = mqtt_port;
  doc["mqtt_user"] = mqtt_user; doc["mqtt_pass"] = mqtt_pass;
  doc["admin_code"] = admin_code; doc["ota_url"] = ota_url;
  JsonArray users = doc.createNestedArray("access");
  for (auto &u : accessUsers) { JsonObject o = users.createNestedObject(); o["username"] = u.username; o["tag"] = u.tag; }
  JsonArray jrules = doc.createNestedArray("rules");
  for (auto &r : rules) { JsonObject o = jrules.createNestedObject();
    o["name"] = r.name; o["trigger"] = r.trigger; o["param"] = r.param;
    o["action"] = r.action; o["actparam"] = r.actparam; }
  File f = LittleFS.open("/settings.json", "w"); serializeJson(doc, f); f.close();
}
void loadSettings() {
  File f = LittleFS.open("/settings.json", "r"); if (!f) return;
  DynamicJsonDocument doc(4096); if (deserializeJson(doc, f)) { f.close(); return; }
  device_id = doc["device_id"] | "door" + String(ESP.getChipId(), HEX);
  ssid = doc["ssid"] | ""; password = doc["password"] | "";
  relay_ms = doc["relay_ms"] | 500; wiegand_mode = doc["wiegand_mode"] | "AUTO";
  mqtt_host = doc["mqtt_host"] | ""; mqtt_port = doc["mqtt_port"] | 1883;
  mqtt_user = doc["mqtt_user"] | ""; mqtt_pass = doc["mqtt_pass"] | "";
  admin_code = doc["admin_code"] | ADMIN_CODE; ota_url = doc["ota_url"] | "";
  accessUsers.clear();
  for (auto u : doc["access"] | JsonArray()) accessUsers.push_back({u["username"], u["tag"]});
  rules.clear();
  for (auto r : doc["rules"] | JsonArray()) rules.push_back({r["name"], r["trigger"], r["param"], r["action"], r["actparam"]});
  f.close();
}

// ---- ADMIN AUTH ----
bool checkAdmin(AsyncWebServerRequest *req) {
  if (!req->hasHeader("X-Admin-Code")) return false;
  return req->getHeader("X-Admin-Code")->value() == admin_code;
}

// ---- MQTT ----
void mqttCb(char* topic, byte* payload, unsigned int len) {
  String t(topic); String pl = String((char*)payload).substring(0, len);
  addLog("MQTT [" + t + "]: " + pl);
  String base = "doors/" + device_id + "/";
  if (t == base + "open") { triggerRelay(); }
  else if (t == base + "lock") { digitalWrite(RELAY_PIN, LOW); addLog("Locked"); }
  else if (t.startsWith(base + "addaccess/")) {
    int x = t.indexOf('/', base.length() + 9);
    String user = t.substring(base.length() + 9, x), tag = t.substring(x + 1);
    accessUsers.push_back({user, tag}); saveSettings(); addLog("User " + user + " added (MQTT)");
  }
  else if (t.startsWith(base + "deleteaccess/")) {
    String user = t.substring(base.length() + 13);
    accessUsers.erase(std::remove_if(accessUsers.begin(), accessUsers.end(),
      [&](AccessUser &u) { return u.username == user; }), accessUsers.end());
    saveSettings(); addLog("User " + user + " deleted (MQTT)");
  }
}
void mqttConnect() {
  if (mqtt_host.length() == 0) return;
  if (!mqtt.connected()) {
    mqtt.setServer(mqtt_host.c_str(), mqtt_port);
    mqtt.setCallback(mqttCb);
    if (mqtt.connect(device_id.c_str(), mqtt_user.c_str(), mqtt_pass.c_str())) {
      String base = "doors/" + device_id + "/";
      mqtt.subscribe((base + "open").c_str());
      mqtt.subscribe((base + "lock").c_str());
      mqtt.subscribe((base + "addaccess/+/#").c_str());
      mqtt.subscribe((base + "deleteaccess/+").c_str());
      addLog("MQTT Connected");
    }
  }
}

// ---- WIEGAND ----
void emitWiegand(uint32_t value, uint8_t len) {
  addLog("Wiegand(" + String(len) + "): " + String(value));
  bool found = false;
  for (auto &u : accessUsers) {
    if (u.tag == String(value)) { triggerRelay(); addLog("Access granted for " + u.username); found = true; break; }
  }
  if (!found) addLog("Unknown Wiegand tag " + String(value));
  // TODO: Rule engine, MQTT/HTTP etc.
}

// ---- API/WEB ----
void sendLog(AsyncWebServerRequest *req) {
  String out = "[";
  for (auto &l : logbuf) {
    String safe = l; safe.replace("\"", "'");
    out += "\"" + safe + "\",";
  }
  out += "]";
  req->send(200, "application/json", out);
}
void sendSettings(AsyncWebServerRequest *req) {
  DynamicJsonDocument doc(1024);
  doc["fw_version"] = FW_VERSION;
  doc["device_id"] = device_id; doc["ssid"] = ssid; doc["relay_ms"] = relay_ms;
  doc["wiegand_mode"] = wiegand_mode; doc["admin_code"] = ""; // ikke send kode!
  req->send(200, "application/json", doc.as<String>());
}
void postSettings(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
  if (!checkAdmin(req)) { req->send(401); return; }
  DynamicJsonDocument doc(1024); DeserializationError err = deserializeJson(doc, data, len);
  if (err) { req->send(400); return; }
  device_id = doc["device_id"] | device_id;
  ssid = doc["ssid"] | ssid;
  relay_ms = doc["relay_ms"] | relay_ms;
  wiegand_mode = doc["wiegand_mode"] | wiegand_mode;
  admin_code = doc["admin_code"] | admin_code;
  saveSettings();
  req->send(200, "application/json", "{\"ok\":true}");
  addLog("Settings updated");
  ESP.restart();
}
void sendStatus(AsyncWebServerRequest *req) {
  DynamicJsonDocument doc(256);
  doc["albue"] = digitalRead(ALBUE_PIN) == LOW;
  doc["door"] = digitalRead(DOOR_PIN) == LOW;
  doc["relay"] = digitalRead(RELAY_PIN) == HIGH;
  req->send(200, "application/json", doc.as<String>());
}
void postRelay(AsyncWebServerRequest *req) {
  if (!checkAdmin(req)) { req->send(401); return; }
  triggerRelay();
  req->send(200, "application/json", "{\"ok\":true}");
}
void sendUsers(AsyncWebServerRequest *req) {
  DynamicJsonDocument doc(1024);
  JsonArray arr = doc.to<JsonArray>();
  for (auto &u : accessUsers) { JsonObject o = arr.createNestedObject(); o["username"] = u.username; o["tag"] = u.tag; }
  req->send(200, "application/json", doc.as<String>());
}
void postUsers(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
  if (!checkAdmin(req)) { req->send(401); return; }
  DynamicJsonDocument doc(128); if (deserializeJson(doc, data, len)) { req->send(400); return; }
  String user = doc["username"], tag = doc["tag"];
  accessUsers.push_back({user, tag}); saveSettings();
  addLog("User " + user + " added (Web)");
  req->send(200, "application/json", "{\"ok\":true}");
}
void deleteUser(AsyncWebServerRequest *req) {
  if (!checkAdmin(req)) { req->send(401); return; }
  String user = req->pathArg(0);
  accessUsers.erase(std::remove_if(accessUsers.begin(), accessUsers.end(),
    [&](AccessUser &u) { return u.username == user; }), accessUsers.end());
  saveSettings();
  addLog("User " + user + " deleted (Web)");
  req->send(200, "application/json", "{\"ok\":true}");
}
// ---- RULES API ----
void sendRules(AsyncWebServerRequest *req) {
  DynamicJsonDocument doc(2048); JsonArray arr = doc.to<JsonArray>();
  for(auto &r:rules) {
    JsonObject o = arr.createNestedObject();
    o["name"] = r.name; o["trigger"] = r.trigger; o["param"] = r.param;
    o["action"] = r.action; o["actparam"] = r.actparam;
  }
  req->send(200, "application/json", doc.as<String>());
}
void postRule(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
  if (!checkAdmin(req)) { req->send(401); return; }
  DynamicJsonDocument doc(256); if(deserializeJson(doc,data,len)){req->send(400);return;}
  rules.push_back({doc["name"],doc["trigger"],doc["param"],doc["action"],doc["actparam"]});
  saveSettings(); req->send(200,"application/json","{\"ok\":true}");
}
void putRule(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
  if (!checkAdmin(req)) { req->send(401); return; }
  String url = req->url();
  int slash = url.lastIndexOf('/');
  int idx = atoi(url.substring(slash+1).c_str());
  DynamicJsonDocument doc(256); if(deserializeJson(doc,data,len)){req->send(400);return;}
  if(idx>=0 && idx<rules.size()) rules[idx] = {doc["name"],doc["trigger"],doc["param"],doc["action"],doc["actparam"]};
  saveSettings(); req->send(200,"application/json","{\"ok\":true}");
}
void deleteRule(AsyncWebServerRequest *req) {
  if (!checkAdmin(req)) { req->send(401); return; }
  String url = req->url();
  int slash = url.lastIndexOf('/');
  int idx = atoi(url.substring(slash+1).c_str());
  if(idx>=0 && idx<rules.size()) rules.erase(rules.begin()+idx);
  saveSettings(); req->send(200,"application/json","{\"ok\":true}");
}


void setup() {
  pinMode(RELAY_PIN, OUTPUT); pinMode(ALBUE_PIN, INPUT_PULLUP); pinMode(DOOR_PIN, INPUT_PULLUP); pinMode(ONBOARD_LED, OUTPUT);
  pinMode(RESET_PIN, INPUT_PULLUP); digitalWrite(RELAY_PIN, LOW);
  Serial.begin(115200); LittleFS.begin(); loadSettings();
  if (ssid == "") { WiFi.mode(WIFI_AP); WiFi.softAP("Door-Setup"); }
  else { WiFi.mode(WIFI_STA); WiFi.begin(ssid.c_str(), password.c_str()); }
  ArduinoOTA.begin();
  wg.begin(WIEGAND_D0, WIEGAND_D1);
  // ---- API endpoints ----
  server.on("/log", HTTP_GET, sendLog);
  server.on("/settings", HTTP_GET, sendSettings);
  server.on("/settings", HTTP_POST, NULL, NULL, postSettings);
  server.on("/status", HTTP_GET, sendStatus);
  server.on("/relay", HTTP_POST, postRelay);
  server.on("/users", HTTP_GET, sendUsers);
  server.on("/users", HTTP_POST, NULL, NULL, postUsers);
  server.on("^\\/users\\/(.+)$", HTTP_DELETE, deleteUser);
  // TODO: add rules/console endpoints
  server.on("/rules", HTTP_GET, sendRules);
server.on("/rules", HTTP_POST, NULL, NULL, postRule);
server.on("^/rules/(\\d+)$", HTTP_PUT, NULL, NULL, putRule);
server.on("^/rules/(\\d+)$", HTTP_DELETE, deleteRule);

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.begin();
  addLog("Door Controller Ready. FW " FW_VERSION);
}

void loop() {
  handleButtonReset();
  ArduinoOTA.handle(); mqtt.loop();
  mqttConnect();
  // Wiegand event
  if (wg.available()) {
    uint8_t bits = wg.getWiegandType();
    uint32_t code = wg.getCode();
    if ((wiegand_mode == "AUTO" && bits >= 26 && bits <= 64)
      || (wiegand_mode == String(bits))) { emitWiegand(code, bits); }
  }
  // Edge: Albuebryter
  static bool prevAlbue = false;
  bool albueNow = digitalRead(ALBUE_PIN) == LOW;
  if (albueNow && !prevAlbue) { triggerRelay(); addLog("Albuebryter trykket"); }
  prevAlbue = albueNow;
  // Edge: Door
  static bool prevDoor = false;
  bool doorNow = digitalRead(DOOR_PIN) == LOW;
  if (doorNow != prevDoor) { addLog(String("Magnetkontakt: ") + (doorNow ? "Lukket" : "Åpen")); }
  prevDoor = doorNow;
  // TODO: rules engine, periodic timers, etc.
  
  delay(10);
}
