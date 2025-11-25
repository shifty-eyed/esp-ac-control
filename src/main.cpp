/*
 * ESP32 AC Control - WiFi HTTP API for Thermostat Interface
 * 
 * Hardware connections:
 *   - GPIO25: Button emulation (drives NPN transistor base via 4.7kΩ)
 *   - GPIO32: LED sense input (digital read, 3V when AC on, 0V when off)
 *   - GND: Shared between ESP32 and thermostat
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

// Global variables for schedules
Schedule schedules[16];
Preferences preferences;

const char* WIFI_SSID     = "imenilenina-bistro";
const char* WIFI_PASSWORD = "10101010";

const int HTTP_PORT = 80;

const int BUTTON_PIN    = 25;
const int LED_SENSE_PIN = 32;

const int BUTTON_PRESS_DURATION = 300;

WebServer server(HTTP_PORT);

bool isAcOn() {
  const int samples = 8;
  
  for (int i = 0; i < samples; i++) {
    if (digitalRead(LED_SENSE_PIN) == HIGH) {
      return true;
    }
    delay(5);
  }
  return false;
}

void pressButton() {
  Serial.println("[HW] Pressing button...");
  digitalWrite(BUTTON_PIN, HIGH);
  delay(BUTTON_PRESS_DURATION);
  digitalWrite(BUTTON_PIN, LOW);
  Serial.println("[HW] Button released");
  delay(100);
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
  
  // Try to sync, but don't block if it fails
  Serial.print("[TIME] Attempting initial sync");
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (attempts < 20) {
    Serial.println();
    Serial.println("[TIME] Time synchronized!");
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.print("[TIME] Current time: ");
    Serial.println(timeStr);
  } else {
    Serial.println();
    Serial.println("[TIME] WARNING: Initial sync failed (AC control will still work)");
  }
}

void manualSyncTime() {
  Serial.println("[TIME] Manual sync requested...");
  sntp_restart();
}

String getCurrentTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "{\"error\": \"Time not available\"}";
  }
  
  char timeStr[64];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  
  bool synced = (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
  
  String json = "{\"time\": \"";
  json += timeStr;
  json += "\", \"synced\": ";
  json += synced ? "true" : "false";
  json += "}";
  
  return json;
}

// ========== Schedule Management Functions ==========

bool isScheduleValid(int id) {
  if (id < 0 || id >= 16) return false;
  return schedules[id].valid;
}

void checkSchedules() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return;  // Time not available yet
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
      
      Serial.print("[SCHEDULE] Triggered schedule ");
      Serial.print(i);
      Serial.print(" at ");
      Serial.print(currentHour);
      Serial.print(":");
      if (currentMinute < 10) Serial.print("0");
      Serial.println(currentMinute);
      
      bool currentState = isAcOn();
      bool desiredState = (schedules[i].switchState == 1);
      
      if (desiredState && !currentState) {
        Serial.println("[SCHEDULE] Turning AC ON");
        pressButton();
      } else if (!desiredState && currentState) {
        Serial.println("[SCHEDULE] Turning AC OFF");
        pressButton();
      } else {
        Serial.print("[SCHEDULE] AC already in desired state: ");
        Serial.println(desiredState ? "ON" : "OFF");
      }
    }
    
    // Reset executed flag when minute changes
    if (schedules[i].minute != currentMinute) {
      schedules[i].executed = false;
    }
  }
}

String schedulesToJson() {
  String json = "[";
  bool first = true;
  
  for (int i = 0; i < 16; i++) {
    if (schedules[i].valid) {
      if (!first) {
        json += ",";
      }
      first = false;
      
      json += "{\"id\":";
      json += i;
      json += ",\"hour\":";
      json += schedules[i].hour;
      json += ",\"minute\":";
      json += schedules[i].minute;
      json += ",\"switch\":";
      json += schedules[i].switchState;
      json += "}";
    }
  }
  
  json += "]";
  return json;
}

void handleStatus() {
  bool acOn = isAcOn();
  String response = acOn ? "1\n" : "0\n";
  
  Serial.print("[HTTP] GET /status → ");
  Serial.println(response);
  
  server.send(200, "text/plain", response);
}

void handleOn() {
  Serial.println("[HTTP] PUT /on");
  
  if (isAcOn()) {
    server.send(200, "text/plain", "OK - Already on\n");
  } else {
    pressButton();
    delay(500);
    
    if (isAcOn()) {
      server.send(200, "text/plain", "OK - Turned on\n");
    } else {
      Serial.println("[HTTP] Warning: AC may not have turned ON");
      server.send(200, "text/plain", "OK - Failed to turn on\n");
    }
  }
}

void handleOff() {
  Serial.println("[HTTP] PUT /off");
  
  if (!isAcOn()) {
    server.send(200, "text/plain", "OK - Already off");
  } else {
    pressButton();
    delay(500);
    if (!isAcOn()) {
      server.send(200, "text/plain", "OK - Turned off");
    } else {
      server.send(200, "text/plain", "OK - Failed to turn off");
    }
  }
}

void handleNotFound() {
  String message = "Not Found\n\n";
  message += "Available endpoints:\n";
  message += "  GET  /status\n";
  message += "  PUT  /on\n";
  message += "  PUT  /off\n";
  message += "  GET  /time\n";
  message += "  PUT  /synctime\n";
  message += "  GET  /schedule\n";
  message += "  PUT  /schedule?id=X&hour=H&minute=M&switch=S\n";
  message += "  DELETE /schedule?id=X\n";
  
  server.send(404, "text/plain", message);
}

// ========== New HTTP Endpoint Handlers ==========

void handleGetTime() {
  Serial.println("[HTTP] GET /time");
  String response = getCurrentTimeString();
  server.send(200, "application/json", response);
}

void handleSyncTime() {
  Serial.println("[HTTP] PUT /synctime");
  
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"error\": \"WiFi not connected\"}");
    return;
  }
  
  manualSyncTime();
  server.send(200, "application/json", "{\"status\": \"syncing\"}");
}

void handleGetSchedule() {
  Serial.println("[HTTP] GET /schedule");
  String response = schedulesToJson();
  server.send(200, "application/json", response);
}

void handlePutSchedule() {
  Serial.println("[HTTP] PUT /schedule");
  
  // Parse query parameters
  if (!server.hasArg("id") || !server.hasArg("hour") || 
      !server.hasArg("minute") || !server.hasArg("switch")) {
    server.send(400, "application/json", 
                "{\"error\": \"Missing parameters. Required: id, hour, minute, switch\"}");
    return;
  }
  
  int id = server.arg("id").toInt();
  int hour = server.arg("hour").toInt();
  int minute = server.arg("minute").toInt();
  int switchState = server.arg("switch").toInt();
  
  // Validate parameters
  if (id < 0 || id >= 16) {
    server.send(400, "application/json", "{\"error\": \"id must be 0-15\"}");
    return;
  }
  if (hour < 0 || hour > 23) {
    server.send(400, "application/json", "{\"error\": \"hour must be 0-23\"}");
    return;
  }
  if (minute < 0 || minute > 59) {
    server.send(400, "application/json", "{\"error\": \"minute must be 0-59\"}");
    return;
  }
  if (switchState != 0 && switchState != 1) {
    server.send(400, "application/json", "{\"error\": \"switch must be 0 or 1\"}");
    return;
  }
  
  // Update schedule
  schedules[id].id = id;
  schedules[id].hour = hour;
  schedules[id].minute = minute;
  schedules[id].switchState = switchState;
  schedules[id].executed = false;
  schedules[id].valid = true;
  
  // Save to NVS
  saveScheduleToNVS(id);
  
  Serial.print("[HTTP] Created/updated schedule ");
  Serial.print(id);
  Serial.print(": ");
  Serial.print(hour);
  Serial.print(":");
  if (minute < 10) Serial.print("0");
  Serial.print(minute);
  Serial.print(" switch=");
  Serial.println(switchState);
  
  String response = "{\"status\": \"ok\", \"id\": ";
  response += id;
  response += "}";
  
  server.send(200, "application/json", response);
}

void handleDeleteSchedule() {
  Serial.println("[HTTP] DELETE /schedule");
  
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\": \"Missing id parameter\"}");
    return;
  }
  
  int id = server.arg("id").toInt();
  
  if (id < 0 || id >= 16) {
    server.send(400, "application/json", "{\"error\": \"id must be 0-15\"}");
    return;
  }
  
  if (!schedules[id].valid) {
    server.send(404, "application/json", "{\"error\": \"Schedule not found\"}");
    return;
  }
  
  deleteScheduleFromNVS(id);
  
  Serial.print("[HTTP] Deleted schedule ");
  Serial.println(id);
  
  String response = "{\"status\": \"deleted\", \"id\": ";
  response += id;
  response += "}";
  
  server.send(200, "application/json", response);
}

void setup() {
  Serial.begin(9600);
  delay(100);
  
  pinMode(BUTTON_PIN, OUTPUT);
  digitalWrite(BUTTON_PIN, LOW);
  
  pinMode(LED_SENSE_PIN, INPUT);
  
  bool initialState = isAcOn();
  Serial.print("[HW] Initial AC state: ");
  Serial.println(initialState ? "ON" : "OFF");
  
  // Load schedules from NVS
  loadSchedulesFromNVS();
  
  // Connect to WiFi
  Serial.print("[WiFi] Connecting to: ");
  Serial.println(WIFI_SSID);
  
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
  Serial.print("[WiFi] Signal strength (RSSI): ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
  
  // Initialize time synchronization
  initTime();
  
  // Configure HTTP server routes
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/on", HTTP_PUT, handleOn);      
  server.on("/off", HTTP_PUT, handleOff);
  server.on("/time", HTTP_GET, handleGetTime);
  server.on("/synctime", HTTP_PUT, handleSyncTime);
  server.on("/schedule", HTTP_GET, handleGetSchedule);
  server.on("/schedule", HTTP_PUT, handlePutSchedule);
  server.on("/schedule", HTTP_DELETE, handleDeleteSchedule);
  server.onNotFound(handleNotFound);
  
  // Start HTTP server
  server.begin();
  Serial.println();
  Serial.print("[HTTP] Server started on port ");
  Serial.println(HTTP_PORT);
  Serial.println();
  Serial.println("Ready! Test with:");
  Serial.print("  curl http://");
  Serial.print(WiFi.localIP());
  Serial.println("/status");
  Serial.print("  curl -X PUT http://");
  Serial.print(WiFi.localIP());
  Serial.println("/on");
  Serial.print("  curl -X PUT http://");
  Serial.print(WiFi.localIP());
  Serial.println("/off");
  Serial.println();
}

void loop() {
  server.handleClient();
  checkSchedules();  // Check if any schedules need to be triggered
  delay(2);
}
