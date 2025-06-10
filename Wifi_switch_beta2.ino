// In Arduino IDE go to Tools> MMU (It is after Debug Level)> Change to 16KB cache + 48KB IRAM(IRAM).
// Either create a credentials.h file with the needed credentials or hardcode the credentials directly into code as strings.
#include "credentials.h"
#include "DHT20.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <digitalWriteFast.h>

// ==================== GLOBALS ====================
DHT20 DHT;
ESP8266WebServer server(80);
WiFiUDP udp;
NTPClient timeClient(udp, "time.google.com", 19800);
U8G2_SSD1306_128X32_UNIVISION_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

enum DisplayMode { SHOW_AVG_TEMP, SHOW_AVG_HUMIDITY, SHOW_HEAT_INDEX, SHOW_PRESSURE, SHOW_TVOC };
DisplayMode currentMode = SHOW_AVG_TEMP;

struct Output {
  uint8_t pin;
  String state;
  String name;
};

struct DeviceTimer {
  unsigned long endTime = 0;
  String targetState = "";
  bool active = false;
};

// Device outputs
Output outputs[] = {
  { 16, "On", "Fan" },          // D0
  { 15, "Off", "Big Light" },   // D8
  { 12, "Off", "Light" },       // D6
  { 14, "On", "Low Speed" },    // D5 (Fan Low Speed)
};

DeviceTimer deviceTimers[3]; // Fan, Big Light, Light
const uint8_t touchPin = 13;  // D7
const String clockIP = CLOCK_IP;

// Timing variables with staggered intervals to prevent collision
unsigned long lastSensorTime = 0, lastDisplayTime = 0, lastProcessTime = 0, 
              lastBrightnessCheck = 0, lastReconnectTime = 0, lastRemoteSensorTime = 0,
              lastVOCCheckTime = 0, lastTimeUpdateTime = 0;

// Non-blocking HTTP state management
enum HttpState { HTTP_IDLE, HTTP_REQUESTING, HTTP_PROCESSING };
struct HttpRequest {
  HttpState state = HTTP_IDLE;
  String url = "";
  String method = "GET";
  String postData = "";
  unsigned long startTime = 0;
  unsigned long timeout = 5000; // 5 second timeout
  bool expectResponse = false;
  String response = "";
};

HttpRequest remoteDataRequest;
HttpRequest vocControlRequest;
HttpRequest clockControlRequest;

// Sensor & control variables
float sensorTemperature = 0, sensorHumidity = 0, heatIndex = 0, 
      remoteTemp = 0, remoteHumid = 0, remotePressure = 0,
      avgTemp = 0, avgHumid = 0, avgHeatIndex = 0, displayedPressure = 0, displayedTVOC = 0;

uint32_t remoteTVOC = 0;
uint8_t displayBrightness = 60;
bool autoBrightness = true, autoVOCControl = true, relayTriggeredByVOC = false;
bool pendingVOCAction = false;
bool pendingVOCState = false;

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  setupWiFiSensorsDisplay();
  setupWebServer();
}

// ==================== MAIN LOOP ====================
void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  // Handle non-blocking HTTP requests
  processHttpRequests();

  unsigned long now = millis();
  
  // High frequency tasks (50ms)
  if (now - lastProcessTime > 50) {
    checkTouchButton();
    lastProcessTime = now;
  }
  
  // Medium frequency tasks (3s) - staggered
  if (now - lastDisplayTime > 3000) {
    updateDisplay();
    checkTimer();
    lastDisplayTime = now;
  }
  
  // Local sensor reading (10s)
  if (now - lastSensorTime > 10000) {
    handleLocalSensorData();
    lastSensorTime = now;
  }
  
  // Remote sensor data (12s) - offset to avoid collision
  if (now - lastRemoteSensorTime > 12000) {
    requestRemoteSensorData();
    lastRemoteSensorTime = now;
  }
  
  // VOC level checking (8s) - different interval
  if (now - lastVOCCheckTime > 8000) {
    checkVOCLevelsNonBlocking();
    lastVOCCheckTime = now;
  }
  
  // Time update (60s)
  if (now - lastTimeUpdateTime > 60000) {
    timeClient.update();
    lastTimeUpdateTime = now;
  }
  
  // Brightness check (30s)
  if (autoBrightness && now - lastBrightnessCheck > 30000) {
    autoBrightnessCheck();
    lastBrightnessCheck = now;
  }
  
  // WiFi reconnection check (15s) - different interval
  if (now - lastReconnectTime > 15000) {
    maintainWiFiConnection();
    lastReconnectTime = now;
  }
  
  yield(); // Allow ESP8266 to handle background tasks
}

// ==================== NON-BLOCKING HTTP MANAGEMENT ====================
void processHttpRequests() {
  // Process remote sensor data request
  if (remoteDataRequest.state == HTTP_REQUESTING) {
    processHttpRequest(&remoteDataRequest);
    if (remoteDataRequest.state == HTTP_IDLE && !remoteDataRequest.response.isEmpty()) {
      parseRemoteSensorData(remoteDataRequest.response);
      remoteDataRequest.response = "";
    }
  }
  
  // Process VOC control request
  if (vocControlRequest.state == HTTP_REQUESTING) {
    processHttpRequest(&vocControlRequest);
    if (vocControlRequest.state == HTTP_IDLE) {
      pendingVOCAction = false;
    }
  }
  
  // Process clock control request
  if (clockControlRequest.state == HTTP_REQUESTING) {
    processHttpRequest(&clockControlRequest);
    if (clockControlRequest.state == HTTP_IDLE) {
      // Clock control completed
    }
  }
}

void processHttpRequest(HttpRequest* request) {
  static WiFiClient client;
  static HTTPClient http;
  
  if (millis() - request->startTime > request->timeout) {
    // Timeout occurred
    if (http.connected()) {
      http.end();
    }
    request->state = HTTP_IDLE;
    Serial.println("HTTP request timeout: " + request->url);
    return;
  }
  
  if (!http.connected()) {
    // Start the request
    if (!http.begin(client, request->url)) {
      request->state = HTTP_IDLE;
      Serial.println("HTTP begin failed: " + request->url);
      return;
    }
    
    if (request->method == "POST") {
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      int httpCode = http.POST(request->postData);
      if (request->expectResponse && httpCode == HTTP_CODE_OK) {
        request->response = http.getString();
      }
    } else {
      int httpCode = http.GET();
      if (request->expectResponse && httpCode == HTTP_CODE_OK) {
        request->response = http.getString();
      }
    }
    
    http.end();
    request->state = HTTP_IDLE;
  }
}

void startHttpRequest(HttpRequest* request, String url, String method = "GET", String postData = "", bool expectResponse = false) {
  if (request->state != HTTP_IDLE) {
    return; // Request already in progress
  }
  
  request->url = url;
  request->method = method;
  request->postData = postData;
  request->expectResponse = expectResponse;
  request->startTime = millis();
  request->state = HTTP_REQUESTING;
  request->response = "";
}

// ==================== INITIALIZATION ====================
void setupWiFiSensorsDisplay() {
  // WiFi setup
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  WiFi.begin(STA_SSID, STA_PASSWORD);
  
  Serial.print("Connecting to WiFi");
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
    yield(); // Allow background tasks during connection
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? 
    "\nConnected! IP: " + WiFi.localIP().toString() : "\nConnection failed");
  
  // Services
  MDNS.begin("HOME");
  MDNS.addService("http", "tcp", 80);
  
  // OTA setup
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
  
  // Hardware initialization
  Wire.begin();
  udp.begin(123);
  DHT.begin();
  
  // OLED setup
  u8g2.begin();
  displayCenteredText("JARVIS");
  delay(2000);
  
  // Pin initialization
  pinMode(touchPin, INPUT_PULLUP);
  for (auto& out : outputs) {
    pinMode(out.pin, OUTPUT);
    if (out.name == "Fan") updateFanState(out.state);
    else digitalWriteFast(out.pin, out.state == "On" ? HIGH : LOW);
  }
  
  // Time & sensors
  timeClient.begin();
  timeClient.forceUpdate();
  handleLocalSensorData();
  setOledBrightness(timeClient.getHours() < 7 ? 22 : 60);
}

void setupWebServer() {
  // Device control endpoints
  server.on("/reset", handleReset);
  server.on("/fan/toggle", handleFanControl);
  server.on("/output/1/toggle", []() { toggleOutput(1); });
  server.on("/output/2/toggle", []() { toggleOutput(2); });
  
  // Data endpoints
  server.on("/sensorData", handleSensorDataAPI);
  server.on("/deviceStates", handleDeviceStates);
  server.on("/device/info", handleDeviceInfo);
  server.on("/api/time", handleTimeAPI);
  
  // Display control
  server.on("/brightness", handleBrightnessControl);
  server.on("/brightness/auto", handleAutoBrightness);
  
  // Timer endpoints
  server.on("/timer/status", handleTimerStatus);
  server.on("/timer", HTTP_POST, handleTimer);
  server.on("/timer/cancel", HTTP_POST, handleTimerCancel);
  
  // Clock device proxy endpoints
  setupClockEndpoints();
  
  server.begin();
}

void setupClockEndpoints() {
  server.on("/voc/auto", handleVOCControl);
  server.on("/clock/relay", []() {
    if (server.hasArg("state")) {
      controlClockRelayNonBlocking(server.arg("state") == "on");
    }
    server.send(200, "text/plain", "OK");
  });
  
  server.on("/clock/brightness", HTTP_GET, []() {
    forwardClockRequestSync("/brightness/get");
  });
  
  server.on("/clock/brightness", HTTP_POST, []() {
    if (server.hasArg("value")) {
      setClockBrightnessNonBlocking(server.arg("value").toInt());
    }
    server.send(200, "text/plain", "OK");
  });
  
  server.on("/clock/brightness/auto", HTTP_GET, []() {
    String response = httpGetStringSync("http://" + clockIP + "/brightness/get");
    if (!response.isEmpty()) {
      DynamicJsonDocument doc(256);
      deserializeJson(doc, response);
      server.send(200, "text/plain", doc["auto"] ? "on" : "off");
    } else {
      server.send(500, "text/plain", "Error");
    }
  });
  
  server.on("/clock/brightness/auto", HTTP_POST, []() {
    if (server.hasArg("state")) {
      startHttpRequest(&clockControlRequest, 
                      "http://" + clockIP + "/brightness/auto", 
                      "POST", 
                      "state=" + server.arg("state"));
      server.send(200, "text/plain", "OK");
    }
  });
  
  server.on("/clock/relay/status", HTTP_GET, []() {
    String response = httpGetStringSync("http://" + clockIP + "/relay/state");
    server.send(response.isEmpty() ? 500 : 200, "text/plain", 
                response.isEmpty() ? "Error" : response);
  });
}

// ==================== CORE FUNCTIONS ====================
void autoBrightnessCheck() {
  int hour = timeClient.getHours();
  uint8_t newBrightness = (hour > 22 || hour < 7) ? 0 : 60;
  
  if (newBrightness != displayBrightness) {
    displayBrightness = newBrightness;
    setOledBrightness(displayBrightness);
  }
}

void updateDisplay() {
  char buffer[16];
  char label[16];
  
  switch (currentMode) {
    case SHOW_AVG_TEMP:     
      snprintf(label, 16, "Temperature:");
      snprintf(buffer, 16, "%.1fC", avgTemp); 
      break;
    case SHOW_AVG_HUMIDITY: 
      snprintf(label, 16, "Humidity:");
      snprintf(buffer, 16, "%.1f%%", avgHumid); 
      break;
    case SHOW_HEAT_INDEX:   
      snprintf(label, 16, "Feels like:");
      snprintf(buffer, 16, "%.1fC", avgHeatIndex); 
      break;
    case SHOW_PRESSURE:     
      snprintf(label, 16, "Pressure:");
      snprintf(buffer, 16, "%.1f", displayedPressure); 
      break;
    case SHOW_TVOC:         
      snprintf(label, 16, "TVOC:");
      snprintf(buffer, 16, "%.2fPPM", displayedTVOC/1000.0); 
      break;
  }
  
  u8g2.firstPage();
  do {
    // Draw small label at top
    u8g2.setFont(u8g2_font_siji_t_6x10);  // Small font for label
    u8g2.drawStr(0, 7, label);
    
    // Draw big data value below
    u8g2.setFont(u8g2_font_inr21_mr);  // Slightly smaller than helvB24 to fit
    int dataX = (128 - u8g2.getStrWidth(buffer)) / 2;
    u8g2.drawStr(dataX, 32, buffer);  // Position below label
    
  } while (u8g2.nextPage());
  
  currentMode = static_cast<DisplayMode>((currentMode + 1) % 5);
}

void checkTouchButton() {
  static bool lastState = HIGH;
  bool currentState = digitalReadFast(touchPin);
  
  if (currentState == HIGH && lastState == LOW) {
    // Toggle lights
    for (int i = 1; i <= 2; i++) {
      digitalWriteFast(outputs[i].pin, !digitalReadFast(outputs[i].pin));
      outputs[i].state = digitalReadFast(outputs[i].pin) ? "On" : "Off";
    }
  }
  lastState = currentState;
}

void checkTimer() {
  unsigned long now = millis();
  
  for (int i = 0; i < 3; i++) {
    if (deviceTimers[i].active && now >= deviceTimers[i].endTime) {
      // Execute timer action
      if (i == 0) {
        updateFanState(deviceTimers[i].targetState);
      } else {
        outputs[i].state = deviceTimers[i].targetState;
        digitalWriteFast(outputs[i].pin, deviceTimers[i].targetState == "On" ? HIGH : LOW);
      }
      
      // Clear timer
      deviceTimers[i] = {}; // Reset to default values
    }
  }
}

void checkVOCLevelsNonBlocking() {
  if (!autoVOCControl || pendingVOCAction) return;
  
  bool shouldActivate = (remoteTVOC >= 1500 || avgHeatIndex >= 45);
  
  if (shouldActivate && !relayTriggeredByVOC) {
    controlClockRelayNonBlocking(true);
    relayTriggeredByVOC = true;
  } else if (!shouldActivate && relayTriggeredByVOC) {
    controlClockRelayNonBlocking(false);
    relayTriggeredByVOC = false;
  }
}

// ==================== SENSOR & DATA ====================
void handleLocalSensorData() {
  if (DHT.read() == DHT20_OK) {
    sensorTemperature = DHT.getTemperature();
    sensorHumidity = DHT.getHumidity();
  }
  
  // Update averages
  updateAverages();
}

void requestRemoteSensorData() {
  if (remoteDataRequest.state == HTTP_IDLE) {
    startHttpRequest(&remoteDataRequest, 
                    "http://" + clockIP + "/sensor/api", 
                    "GET", 
                    "", 
                    true);
  }
}

void parseRemoteSensorData(String response) {
  if (response.isEmpty()) return;
  
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, response) == DeserializationError::Ok) {
    // Only update if values are valid
    if (doc.containsKey("temperature")) remoteTemp = doc["temperature"];
    if (doc.containsKey("humidity")) remoteHumid = doc["humidity"];
    if (doc.containsKey("pressure")) remotePressure = doc["pressure"];
    if (doc.containsKey("tvoc")) remoteTVOC = doc["tvoc"];
    
    // Update averages and derived values
    updateAverages();
  }
}

void updateAverages() {
  avgTemp = (sensorTemperature + remoteTemp) / 2.0;
  avgHumid = (sensorHumidity + remoteHumid) / 2.0;
  
  // Heat index calculation (NOAA formula)
  float tempF = avgTemp * 9.0 / 5.0 + 32;
  float hiF = -42.379 + 2.04901523 * tempF + 10.14333127 * avgHumid
              - 0.22475541 * tempF * avgHumid - 0.00683783 * pow(tempF, 2)
              - 0.05481717 * pow(avgHumid, 2) + 0.00122874 * pow(tempF, 2) * avgHumid
              + 0.00085282 * tempF * pow(avgHumid, 2)
              - 0.00000199 * pow(tempF, 2) * pow(avgHumid, 2);
  if (avgHumid < 13.0 && tempF >= 80.0 && tempF <= 112.0) {
      float adjustment = ((13.0 - avgHumid) / 4.0) * sqrt((17.0 - abs(tempF - 95.0)) / 17.0);
      hiF -= adjustment;
  }
  if (avgHumid > 85.0 && tempF >= 80.0 && tempF <= 87.0) {
      float adjustment = ((avgHumid - 85.0) / 10.0) * ((87.0 - tempF) / 5.0);
      hiF += adjustment;
  }
  avgHeatIndex = (hiF - 32) * 5.0 / 9.0;
  
  displayedPressure = remotePressure;
  displayedTVOC = remoteTVOC;
}

void handleSensorDataAPI() {
  // This is for API response
  String json = createSensorJSON();
  server.send(200, "application/json; charset=utf-8", json);
}

// ==================== WEB HANDLERS ====================
void handleTimer() {
  if (!server.hasArg("device") || !server.hasArg("state") || !server.hasArg("duration")) {
    server.send(400, "text/plain", "Missing parameters");
    return;
  }
  
  String device = server.arg("device");
  int duration = server.arg("duration").toInt();
  
  if (duration <= 0) {
    server.send(400, "text/plain", "Invalid duration");
    return;
  }
  
  int deviceIndex = getDeviceIndex(device);
  if (deviceIndex < 0) {
    server.send(400, "text/plain", "Invalid device");
    return;
  }
  
  deviceTimers[deviceIndex] = {
    millis() + (duration * 60000UL),
    server.arg("state"),
    true
  };
  
  server.send(200, "text/plain", "Timer set for " + String(duration) + " minutes");
}

void handleTimerCancel() {
  if (server.hasArg("device")) {
    int deviceIndex = getDeviceIndex(server.arg("device"));
    if (deviceIndex >= 0) {
      deviceTimers[deviceIndex] = {};
      server.send(200, "text/plain", "Timer cancelled");
    } else {
      server.send(400, "text/plain", "Invalid device");
    }
  } else {
    // Cancel all timers
    for (int i = 0; i < 3; i++) deviceTimers[i] = {};
    server.send(200, "text/plain", "All timers cancelled");
  }
}

void handleTimerStatus() {
  DynamicJsonDocument doc(512);
  JsonArray timers = doc.createNestedArray("timers");
  
  for (int i = 0; i < 3; i++) {
    if (deviceTimers[i].active) {
      JsonObject timer = timers.createNestedObject();
      timer["device"] = getDeviceName(i);
      timer["deviceId"] = getDeviceId(i);
      timer["targetState"] = deviceTimers[i].targetState;
      
      unsigned long remaining = deviceTimers[i].endTime - millis();
      timer["remainingMinutes"] = remaining / 60000;
      timer["remainingSeconds"] = (remaining % 60000) / 1000;
      timer["remainingTime"] = 
        String(remaining / 60000) + "m " + 
        String((remaining % 60000) / 1000) + "s";
    }
  }
  doc["hasActiveTimers"] = timers.size() > 0;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleDeviceStates() {
  StaticJsonDocument<128> doc;
  doc["fan"] = outputs[0].state;
  doc["bigLight"] = outputs[1].state;
  doc["light"] = outputs[2].state;
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleDeviceInfo() {
  server.send(200, "application/json", "{\"ip\":\"" + WiFi.localIP().toString() + "\"}");
}

void handleTimeAPI() {
  StaticJsonDocument<128> doc;
  doc["hours"] = timeClient.getHours();
  doc["minutes"] = timeClient.getMinutes();
  doc["seconds"] = timeClient.getSeconds();
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleFanControl() {
  updateFanState(nextFanState(outputs[0].state));
  server.send(200, "application/json", "{\"status\":\"success\"}");
}

void handleAutoBrightness() {
  if (server.hasArg("state")) {
    autoBrightness = (server.arg("state") == "true");
    if (autoBrightness) {
      int hour = timeClient.getHours();
      displayBrightness = (hour > 22 || hour < 7) ? 0 : 60;
      setOledBrightness(displayBrightness);
    }
  }
  server.send(200, "application/json", "{\"auto\":" + String(autoBrightness ? "true" : "false") + "}");
}

void handleBrightnessControl() {
  if (server.hasArg("value")) {
    autoBrightness = false;
    int value = constrain(server.arg("value").toInt(), 0, 100);
    setOledBrightness(value);
    server.send(200, "text/plain", String(value));
  }
}

void handleVOCControl() {
  if (server.hasArg("state")) {
    autoVOCControl = (server.arg("state") == "on");
    if (!autoVOCControl && relayTriggeredByVOC) {
      controlClockRelayNonBlocking(false);
      relayTriggeredByVOC = false;
    }
  }
  server.send(200, "text/plain", autoVOCControl ? "on" : "off");
}

void handleReset() {
  server.send(200, "text/plain", "Restarting...");
  delay(1000);
  ESP.restart();
}

// ==================== UTILITY FUNCTIONS ====================
void toggleOutput(int index) {
  if (index >= 1 && index <= 2) {
    outputs[index].state = (outputs[index].state == "On") ? "Off" : "On";
    digitalWriteFast(outputs[index].pin, outputs[index].state == "On" ? HIGH : LOW);
    server.send(200, "application/json", "{\"status\":\"success\"}");
  }
}

void updateFanState(String state) {
  outputs[0].state = state;
  if (state == "Off") {
    digitalWriteFast(outputs[0].pin, LOW);
    digitalWriteFast(outputs[3].pin, HIGH);
  } else if (state == "On") {
    digitalWriteFast(outputs[0].pin, HIGH);
    digitalWriteFast(outputs[3].pin, HIGH);
  } else { // Low Speed
    digitalWriteFast(outputs[0].pin, LOW);
    digitalWriteFast(outputs[3].pin, LOW);
  }
}

String nextFanState(String current) {
  if (current == "Off") return "On";
  if (current == "On") return "Low Speed";
  return "Off";
}

void setOledBrightness(uint8_t brightness) {
  displayBrightness = brightness;
  uint8_t contrast = map(brightness, 0, 100, 0, 255);
  u8g2.sendF("ca", 0x81, contrast);
}

void displayCenteredText(const char* text) {
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_helvB24_tr);
    int x = (128 - u8g2.getStrWidth(text)) / 2;
    int y = (32 + u8g2.getMaxCharHeight()) / 2;
    u8g2.drawStr(x, y, text);
  } while (u8g2.nextPage());
}

void controlClockRelayNonBlocking(bool state) {
  if (vocControlRequest.state == HTTP_IDLE) {
    pendingVOCAction = true;
    pendingVOCState = state;
    startHttpRequest(&vocControlRequest, 
                    "http://" + clockIP + "/relay/" + (state ? "on" : "off"));
  }
}

void setClockBrightnessNonBlocking(int value) {
  if (clockControlRequest.state == HTTP_IDLE) {
    startHttpRequest(&clockControlRequest, 
                    "http://" + clockIP + "/brightness", 
                    "POST", 
                    "value=" + String(value));
  }
}

void maintainWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, attempting reconnection...");
    WiFi.reconnect();
  }
}

// ==================== SYNCHRONOUS HELPER FUNCTIONS (for immediate web responses) ====================
String httpGetStringSync(String url) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(3000); // 3 second timeout for sync requests
  http.begin(client, url);
  String result = (http.GET() == HTTP_CODE_OK) ? http.getString() : "";
  http.end();
  return result;
}

void forwardClockRequestSync(String endpoint) {
  String response = httpGetStringSync("http://" + clockIP + endpoint);
  server.send(response.isEmpty() ? 500 : 200, "application/json", 
              response.isEmpty() ? "{\"error\":\"failed\"}" : response);
}

// ==================== HELPER FUNCTIONS ====================
int getDeviceIndex(String device) {
  if (device == "fan") return 0;
  if (device == "bigLight") return 1;
  if (device == "light") return 2;
  return -1;
}

String getDeviceName(int index) {
  String names[] = {"Fan", "Big Light", "Light"};
  return (index >= 0 && index < 3) ? names[index] : "";
}

String getDeviceId(int index) {
  String ids[] = {"fan", "bigLight", "light"};
  return (index >= 0 && index < 3) ? ids[index] : "";
}

String createSensorJSON() {
  StaticJsonDocument<256> doc;
  doc["avgTemp"] = avgTemp;
  doc["avgHumid"] = avgHumid;
  doc["heatIndex"] = avgHeatIndex;
  doc["pressure"] = displayedPressure;
  doc["tvoc"] = displayedTVOC;
  doc["brightness"] = displayBrightness;
  doc["autoBrightness"] = autoBrightness;
  
  String json;
  serializeJson(doc, json);
  return json;
}
