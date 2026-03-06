/*
 * ESP32 AC Control - WiFi HTTP API for Thermostat Interface
 * 
 * Hardware connections:
 *   - GPIO25: Relay control (ACTIVE LOW - LOW=ON, HIGH=OFF)
 *   - GPIO32: LED sense input (digital read, 3V when AC on, 0V when off)
 *   - VIN: Relay module power
 *   - GND: Shared ground
 * 
 * HTTP API:
 *   GET  /status  → returns "1" (AC on) or "0" (AC off)
 *   PUT  /on      → turns AC on if currently off
 *   PUT  /off     → turns AC off if currently on
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_sntp.h>
#include <Preferences.h>

#include "LedSense.h"

// Time configuration
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -5 * 3600;      // GMT-5 (Eastern US)
const int   DAYLIGHT_OFFSET_SEC = 0;

// Schedule structure
struct Schedule {
  int id;
  int hour;
  int minute;
  int switchState;  // 1 = turn on, 0 = turn off
  bool executed;
  bool valid;       // true if schedule slot is populated
};

Schedule schedules[16];
Preferences preferences;
String haWebhookUrl = "";

bool lastKnownAcState = false;
unsigned long lastStateCheckMillis = 0;
const unsigned long STATE_CHECK_INTERVAL = 5000;

unsigned long lastWiFiCheckMillis = 0;
const unsigned long WIFI_CHECK_INTERVAL = 30000;

// Journal (in-memory log)
const int JOURNAL_MAX_LINES = 200;
String journal[JOURNAL_MAX_LINES];
int journalCount = 0;
int journalIndex = 0;  // Circular buffer index

struct StateChangeRecord {
  unsigned long timestamp;
  bool state;
};

const int STATE_JOURNAL_MAX_RECORDS = 3000;
StateChangeRecord stateJournal[STATE_JOURNAL_MAX_RECORDS];
int stateJournalCount = 0;

const char* WIFI_SSID     = "imenilenina-bistro";
const char* WIFI_PASSWORD = "10101010";

const int HTTP_PORT = 80;

const int BUTTON_PIN    = 25;
const int LED_SENSE_PIN = 32;

const int BUTTON_PRESS_DURATION = 300;

WebServer server(HTTP_PORT);

// LED sense is encapsulated to keep signal filtering/polarity isolated.
static LedSense ledSence;

static inline bool isAcOn() {
  return ledSence.isOn();
}

// ========== Journal Functions ==========

void addToJournal(String message) {
  struct tm timeinfo;
  String timestamp;

  if (getLocalTime(&timeinfo)) {
    char timeStr[20];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    timestamp = String(timeStr);
  } else {
    timestamp = "NO-TIME";
  }

  journal[journalIndex] = "[" + timestamp + "] " + message;
  journalIndex = (journalIndex + 1) % JOURNAL_MAX_LINES;

  if (journalCount < JOURNAL_MAX_LINES) {
    journalCount++;
  }

  Serial.println("[JOURNAL] " + message);
}

void clearJournal() {
  journalCount = 0;
  journalIndex = 0;
  Serial.println("[JOURNAL] Cleared");
}

void addToStateJournal(bool state) {
  struct tm timeinfo;
  unsigned long timestamp = 0;

  if (getLocalTime(&timeinfo)) {
    timestamp = mktime(&timeinfo);
  }

  stateJournal[stateJournalCount].timestamp = timestamp;
  stateJournal[stateJournalCount].state = state;
  if (stateJournalCount < STATE_JOURNAL_MAX_RECORDS - 1) {
    stateJournalCount++;
  }
}

void clearStateJournal() {
  stateJournalCount = 0;
}

void initGPIO() {
  pinMode(BUTTON_PIN, OUTPUT);
  digitalWrite(BUTTON_PIN, LOW); 
  pinMode(LED_SENSE_PIN, INPUT_PULLUP);
}

void sendHomeAssistantWebhook(bool acState) {
  if (haWebhookUrl == "" || haWebhookUrl.length() == 0) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    addToJournal("HA webhook skipped: WiFi not connected");
    return;
  }

  HTTPClient http;
  http.begin(haWebhookUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  String payload = "{\"status\":\"" + String(acState ? "on" : "off") + "\"}";

  int httpCode = http.POST(payload);

  String logMsg = "HA webhook: " + String(acState ? "on" : "off");
  if (httpCode > 0) {
    logMsg += " -> HTTP " + String(httpCode);
  } else {
    logMsg += " -> FAILED: " + http.errorToString(httpCode);
  }
  addToJournal(logMsg);

  http.end();
}

String setOn(bool desiredState) {
  const int maxAttempts = 5;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (isAcOn() == desiredState) {
      if (attempt == 0) {
        return "Already there\n";
      } else {
        return "Success from " + String(attempt) + " retry\n";
      }
    }

    digitalWrite(BUTTON_PIN, HIGH);
    ledSence.wait(BUTTON_PRESS_DURATION);
    digitalWrite(BUTTON_PIN, LOW);
    ledSence.wait(500);
    if (isAcOn() != desiredState) {
      ledSence.wait(1500);
    }
  }

  return "Failed after " + String(maxAttempts) + " retries\n";
}

// ========== NVS Schedule Storage Functions ==========

void loadSchedulesFromNVS() {
  preferences.begin("schedules", false);  // false = read/write mode
  
  Serial.println("[NVS] Loading schedules from storage...");
  int loadedCount = 0;
  
  for (int i = 0; i < 16; i++) {
    String keyValid = "sch" + String(i) + "_v";
    bool isValid = preferences.getBool(keyValid.c_str(), false);
    
    if (isValid) {
      String keyHour = "sch" + String(i) + "_h";
      String keyMin = "sch" + String(i) + "_m";
      String keySwitch = "sch" + String(i) + "_s";
      
      schedules[i].id = i;
      schedules[i].hour = preferences.getInt(keyHour.c_str(), 0);
      schedules[i].minute = preferences.getInt(keyMin.c_str(), 0);
      schedules[i].switchState = preferences.getInt(keySwitch.c_str(), 0);
      schedules[i].executed = false;
      schedules[i].valid = true;
      
      loadedCount++;
      Serial.print("[NVS] Loaded schedule ");
      Serial.print(i);
      Serial.print(": ");
      Serial.print(schedules[i].hour);
      Serial.print(":");
      if (schedules[i].minute < 10) Serial.print("0");
      Serial.print(schedules[i].minute);
      Serial.print(" switch=");
      Serial.println(schedules[i].switchState);
    } else {
      schedules[i].valid = false;
      schedules[i].executed = false;
    }
  }
  
  Serial.print("[NVS] Loaded ");
  Serial.print(loadedCount);
  Serial.println(" schedules");
  
  preferences.end();
}

void loadWebhookUrlFromNVS() {
  preferences.begin("config", false);
  haWebhookUrl = preferences.getString("ha_webhook", "");
  preferences.end();

  if (haWebhookUrl.length() > 0) {
    Serial.print("[NVS] Loaded webhook URL: ");
    Serial.println(haWebhookUrl);
  }
}

void saveScheduleToNVS(int id) {
  if (id < 0 || id >= 16) return;
  
  preferences.begin("schedules", false);
  
  String keyValid = "sch" + String(id) + "_v";
  String keyHour = "sch" + String(id) + "_h";
  String keyMin = "sch" + String(id) + "_m";
  String keySwitch = "sch" + String(id) + "_s";
  
  preferences.putBool(keyValid.c_str(), schedules[id].valid);
  preferences.putInt(keyHour.c_str(), schedules[id].hour);
  preferences.putInt(keyMin.c_str(), schedules[id].minute);
  preferences.putInt(keySwitch.c_str(), schedules[id].switchState);
  
  Serial.print("[NVS] Saved schedule ");
  Serial.println(id);
  
  preferences.end();
}

void deleteScheduleFromNVS(int id) {
  if (id < 0 || id >= 16) return;
  
  preferences.begin("schedules", false);
  
  String keyValid = "sch" + String(id) + "_v";
  preferences.putBool(keyValid.c_str(), false);
  
  Serial.print("[NVS] Deleted schedule ");
  Serial.println(id);
  
  preferences.end();
  
  schedules[id].valid = false;
  schedules[id].executed = false;
}

void saveWebhookUrlToNVS(String url) {
  preferences.begin("config", false);
  preferences.putString("ha_webhook", url);
  preferences.end();
  haWebhookUrl = url;

  Serial.print("[NVS] Saved webhook URL: ");
  Serial.println(url);
}

// ========== Time Synchronization Functions ==========

void initTime() {
  Serial.println("[TIME] Initializing NTP time sync...");
  
  // Configure for manual sync only (no automatic re-sync)
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  
  // Configure time with NTP server
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
}

void manualSyncTime() {
  Serial.println("[TIME] Manual sync requested...");
  sntp_restart();
}


// ========== Schedule Management Functions ==========

bool isScheduleValid(int id) {
  if (id < 0 || id >= 16) return false;
  return schedules[id].valid;
}

void checkWiFiConnection() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastWiFiCheckMillis < WIFI_CHECK_INTERVAL) return;
  lastWiFiCheckMillis = currentMillis;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected - reconnecting...");
    WiFi.reconnect();
  }
}

void checkForExternalStateChange() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastStateCheckMillis >= STATE_CHECK_INTERVAL) {
    lastStateCheckMillis = currentMillis;

    bool currentAcState = isAcOn();

    if (currentAcState != lastKnownAcState) {
          lastKnownAcState = currentAcState;
          addToStateJournal(currentAcState);

          sendHomeAssistantWebhook(currentAcState);
    }
  }
}

void checkSchedules() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return;
  }

  int currentHour = timeinfo.tm_hour;
  int currentMinute = timeinfo.tm_min;

  for (int i = 0; i < 16; i++) {
    if (!schedules[i].valid) continue;

    // Check if current time matches schedule
    if (schedules[i].hour == currentHour &&
        schedules[i].minute == currentMinute &&
        !schedules[i].executed) {

      schedules[i].executed = true;

      String action = (schedules[i].switchState == 1) ? "ON" : "OFF";
      String logMsg = "Schedule #" + String(i) + " triggered: Turn " + action;
      addToJournal(logMsg);

      String result = setOn(schedules[i].switchState == 1);
      addToJournal("Schedule #" + String(i) + " result: " + result);
    }

    // Reset executed flag when minute changes
    if (schedules[i].minute != currentMinute) {
      schedules[i].executed = false;
    }
  }
}


void handleStatus() {
  bool acOn = isAcOn();

  // Build combined JSON response
  String response = "{";

  // 1. AC Status
  response += "\"status\":\"";
  response += acOn ? "1" : "0";
  response += "\",";

  // 2. Time Info
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);

    response += "\"time\":\"";
    response += timeStr;
    response += "\",";
  } else {
    response += "\"time\":null,";
  }

  // 3. Schedules
  response += "\"schedules\":[";
  bool first = true;
  for (int i = 0; i < 16; i++) {
    if (schedules[i].valid) {
      if (!first) response += ",";
      first = false;

      response += "{\"id\":";
      response += i;
      response += ",\"hour\":";
      response += schedules[i].hour;
      response += ",\"minute\":";
      response += schedules[i].minute;
      response += ",\"switch\":";
      response += schedules[i].switchState;
      response += "}";
    }
  }
  response += "],";
  response += "\"ha_webhook\":\"" + haWebhookUrl + "\"";
  response += "}\n";

  server.send(200, "application/json", response);
}

void handleOn() {
  addToJournal("Manual turn ON requested");
  String result = setOn(true);
  addToJournal("Manual turn ON result: " + result);
  server.send(200, "text/plain", result);
}

void handleOff() {
  addToJournal("Manual turn OFF requested");
  String result = setOn(false);
  addToJournal("Manual turn OFF result: " + result);
  server.send(200, "text/plain", result);
}

void handleSetWebhook() {
  if (!server.hasArg("value")) {
    server.send(400, "text/plain", "Missing 'value' parameter\n");
    return;
  }

  String url = server.arg("value");
  saveWebhookUrlToNVS(url);
  addToJournal("HA webhook URL updated");

  server.send(200, "text/plain", "Webhook URL updated: " + url + "\n");
}

void handleNotFound() {
  String message = "Not Found\n\n";
  message += "Available endpoints:\n";
  message += "  GET  /status\n";
  message += "  PUT  /on\n";
  message += "  PUT  /off\n";
  message += "  PUT  /synctime\n";
  message += "  PUT  /schedule?id=X&hour=H&minute=M&switch=S\n";
  message += "  DELETE /schedule?id=X\n";
  message += "  GET  /journal\n";
  message += "  DELETE /journal\n";
  message += "  GET  /state-journal\n";
  message += "  DELETE /state-journal\n";

  server.send(404, "text/plain", message);
}

// ========== New HTTP Endpoint Handlers ==========

void handleGetJournal() {
  String response = "";

  int startIdx = (journalCount < JOURNAL_MAX_LINES) ? 0 : journalIndex;

  for (int i = 0; i < journalCount; i++) {
    int idx = (startIdx + i) % JOURNAL_MAX_LINES;
    response += journal[idx];
    response += "\n";
  }
  server.send(200, "text/plain", response);
}

void handleDeleteJournal() {
  clearJournal();
  server.send(200, "application/json", "{\"status\": \"cleared\"}\n");
}

void handleGetStateJournal() {
  String response = "";

  for (int i = 0; i < stateJournalCount; i++) {
    response += stateJournal[i].timestamp;
    response += ",";
    response += stateJournal[i].state ? "1" : "0";
    response += "\n";
  }

  server.send(200, "text/plain", response);
}

void handleDeleteStateJournal() {
  clearStateJournal();
  server.send(200, "application/json", "{\"status\": \"cleared\"}\n");
}

void handleSyncTime() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"error\": \"WiFi not connected\"}\n");
    return;
  }

  manualSyncTime();
  server.send(200, "application/json", "{\"status\": \"syncing\"}\n");
}

void handlePutSchedule() {
  if (!server.hasArg("id") || !server.hasArg("hour") || 
      !server.hasArg("minute") || !server.hasArg("switch")) {
    server.send(400, "application/json", 
                "{\"error\": \"Missing parameters. Required: id, hour, minute, switch\"}\n");
    return;
  }
  
  int id = server.arg("id").toInt();
  int hour = server.arg("hour").toInt();
  int minute = server.arg("minute").toInt();
  int switchState = server.arg("switch").toInt();
  
  if (id < 0 || id >= 16) {
    server.send(400, "application/json", "{\"error\": \"id must be 0-15\"}\n");
    return;
  }
  if (hour < 0 || hour > 23) {
    server.send(400, "application/json", "{\"error\": \"hour must be 0-23\"}\n");
    return;
  }
  if (minute < 0 || minute > 59) {
    server.send(400, "application/json", "{\"error\": \"minute must be 0-59\"}\n");
    return;
  }
  if (switchState != 0 && switchState != 1) {
    server.send(400, "application/json", "{\"error\": \"switch must be 0 or 1\"}\n");
    return;
  }
  
  schedules[id].id = id;
  schedules[id].hour = hour;
  schedules[id].minute = minute;
  schedules[id].switchState = switchState;
  schedules[id].executed = false;
  schedules[id].valid = true;
  
  saveScheduleToNVS(id);
  
  String response = "{\"status\": \"ok\", \"id\": ";
  response += id;
  response += "}\n";
  
  server.send(200, "application/json", response);
}

void handleDeleteSchedule() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\": \"Missing id parameter\"}\n");
    return;
  }
  
  int id = server.arg("id").toInt();
  
  if (id < 0 || id >= 16) {
    server.send(400, "application/json", "{\"error\": \"id must be 0-15\"}\n");
    return;
  }
  
  if (!schedules[id].valid) {
    server.send(404, "application/json", "{\"error\": \"Schedule not found\"}\n");
    return;
  }
  
  deleteScheduleFromNVS(id);
  
  String response = "{\"status\": \"deleted\", \"id\": ";
  response += id;
  response += "}\n";
  
  server.send(200, "application/json", response);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  
  initGPIO();
  ledSence.begin(LED_SENSE_PIN);

  loadSchedulesFromNVS();
  loadWebhookUrlFromNVS();
  
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;

    if (attempts > 60) {  // 30 seconds timeout
      Serial.println();
      Serial.println("[WiFi] Initial connect timeout - will retry in background.");
      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Connected!");
    Serial.print("[WiFi] IP address: ");
    Serial.println(WiFi.localIP());
  }
  
  initTime();
  
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/on", HTTP_PUT, handleOn);
  server.on("/off", HTTP_PUT, handleOff);
  server.on("/ha-hook-url", HTTP_PUT, handleSetWebhook);
  server.on("/synctime", HTTP_PUT, handleSyncTime);
  server.on("/schedule", HTTP_PUT, handlePutSchedule);
  server.on("/schedule", HTTP_DELETE, handleDeleteSchedule);
  server.on("/journal", HTTP_GET, handleGetJournal);
  server.on("/journal", HTTP_DELETE, handleDeleteJournal);
  server.on("/state-journal", HTTP_GET, handleGetStateJournal);
  server.on("/state-journal", HTTP_DELETE, handleDeleteStateJournal);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println();
  Serial.print("[HTTP] Server started on port ");
  Serial.println(HTTP_PORT);
  Serial.println();

  // Initialize the state monitoring
  lastKnownAcState = isAcOn();
  lastStateCheckMillis = millis();
}

void loop() {
  ledSence.update();
  server.handleClient();
  checkSchedules();
  checkForExternalStateChange();
  checkWiFiConnection();
  delay(20);
}

//http://192.168.4.199:8123/api/webhook/ac_status_main
//curl -X PUT "http://192.168.4.120/ha-hook-url?value=http://192.168.4.199:8123/api/webhook/ac_status_main"
//curl -X PUT "http://192.168.4.120/schedule?id=1&hour=7&minute=0&switch=0"
//curl "http://192.168.4.120/state-journal"

//192.168.4.136
//curl -X PUT "http://192.168.4.136/ha-hook-url?value=http://192.168.4.199:8123/api/webhook/ac_status_lower"