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
  
  server.send(404, "text/plain", message);
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
  
  // Configure HTTP server routes
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/on", HTTP_PUT, handleOn);      
  server.on("/off", HTTP_PUT, handleOff);
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
  delay(2);
}
