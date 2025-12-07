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
#include <time.h>
#include <esp_sntp.h>
#include <Preferences.h>

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

// Journal (in-memory log)
const int JOURNAL_MAX_LINES = 300;
String journal[JOURNAL_MAX_LINES];
int journalCount = 0;
int journalIndex = 0;  // Circular buffer index

const char* WIFI_SSID     = "imenilenina-bistro";
const char* WIFI_PASSWORD = "10101010";

const int HTTP_PORT = 80;

const int BUTTON_PIN    = 25;
const int LED_SENSE_PIN = 32;

const int BUTTON_PRESS_DURATION = 300;

WebServer server(HTTP_PORT);

//
//curl -X PUT "http://192.168.4.120/schedule?id=1&hour=7&minute=0&switch=0"

bool isAcOn() {
  for (int i = 0; i < 5; i++) {
    if (digitalRead(LED_SENSE_PIN) == LOW) {
      return true;
    }
    delay(5);
  }
  return false;
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

void initGPIO() {
  pinMode(BUTTON_PIN, OUTPUT);
  digitalWrite(BUTTON_PIN, LOW); 
  pinMode(LED_SENSE_PIN, INPUT_PULLUP);
  
}

String setOn(bool desiredState) {
  const int maxAttempts = 5;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (isAcOn() == desiredState) {
      return attempt == 0 ? "Already there\n" : "Success from " + String(attempt) + " retry\n";
    }

    digitalWrite(BUTTON_PIN, HIGH);
    delay(BUTTON_PRESS_DURATION);
    digitalWrite(BUTTON_PIN, LOW);
    delay(500);
    if (isAcOn() != desiredState) {
      delay(1500);
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
  response += "]";
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

  server.send(404, "text/plain", message);
}

// ========== New HTTP Endpoint Handlers ==========

void handleGetJournal() {
  String response = "[";

  // Read journal in chronological order (oldest to newest)
  int startIdx = (journalCount < JOURNAL_MAX_LINES) ? 0 : journalIndex;

  for (int i = 0; i < journalCount; i++) {
    int idx = (startIdx + i) % JOURNAL_MAX_LINES;

    if (i > 0) response += ",";
    response += "\"";
    response += journal[idx];
    response += "\"";
  }

  response += "]\n";
  server.send(200, "application/json", response);
}

void handleDeleteJournal() {
  clearJournal();
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
  
  loadSchedulesFromNVS();
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;
    
    if (attempts > 60) {  // 30 seconds timeout
      Serial.println();
      Serial.println("[WiFi] ERROR: Connection timeout!");
      Serial.println("[WiFi] Please check credentials and restart.");
      while (true) {
        delay(1000);  // Halt here
      }
    }
  }
  
  Serial.println("[WiFi] Connected!");
  Serial.print("[WiFi] IP address: ");
  Serial.println(WiFi.localIP());
  
  initTime();
  
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/on", HTTP_PUT, handleOn);
  server.on("/off", HTTP_PUT, handleOff);
  server.on("/synctime", HTTP_PUT, handleSyncTime);
  server.on("/schedule", HTTP_PUT, handlePutSchedule);
  server.on("/schedule", HTTP_DELETE, handleDeleteSchedule);
  server.on("/journal", HTTP_GET, handleGetJournal);
  server.on("/journal", HTTP_DELETE, handleDeleteJournal);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println();
  Serial.print("[HTTP] Server started on port ");
  Serial.println(HTTP_PORT);
  Serial.println();
}

void loop() {
  server.handleClient();
  checkSchedules();
  delay(20);
}
