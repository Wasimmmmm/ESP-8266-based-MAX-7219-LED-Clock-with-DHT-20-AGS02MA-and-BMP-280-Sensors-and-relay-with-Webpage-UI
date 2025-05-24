// In Arduino IDE go to Tools> MMU (It is after Debug Level)> Chnage to 16KB cache + 48KB IRAM(IRAM).
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
NTPClient timeClient(udp, "time.google.com", 19800, 3600000);
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

// Timing variables
unsigned long lastSensorTime = 0, lastDisplayTime = 0, lastProcessTime = 0, 
              lastBrightnessCheck = 0, lastReconnectTime = 0;

// Sensor & control variables
float sensorTemperature = 0, sensorHumidity = 0, heatIndex = 0, 
      remoteTemp = 0, remoteHumid = 0, remotePressure = 0,
      avgTemp = 0, avgHumid = 0, avgHeatIndex = 0, displayedPressure = 0;

uint32_t remoteTVOC = 0, displayedTVOC = 0;
uint8_t displayBrightness = 60;
bool autoBrightness = true, autoVOCControl = true, relayTriggeredByVOC = false;

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
  
  unsigned long now = millis();
  
  if (now - lastProcessTime > 50) {
    checkTouchButton();
    lastProcessTime = now;
  }
  
  if (now - lastDisplayTime > 2000) {
    updateDisplay();
    checkTimer();
    lastDisplayTime = now;
  }
  
  if (now - lastSensorTime > 10000) {
    handleSensorData();
    getClockSensorData();
    checkVOCLevels();
    if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); }
    lastSensorTime = now;
  }
  
  if (autoBrightness && now - lastBrightnessCheck > 30000) {
    autoBrightnessCheck();
    lastBrightnessCheck = now;
  }
  
  yield();
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
  handleSensorData();
  setOledBrightness(timeClient.getHours() < 7 ? 22 : 60);
}

void setupWebServer() {
  // Device control endpoints
  server.on("/reset", handleReset);
  server.on("/fan/toggle", handleFanControl);
  server.on("/output/1/toggle", []() { toggleOutput(1); });
  server.on("/output/2/toggle", []() { toggleOutput(2); });
  
  // Data endpoints
  server.on("/sensorData", handleSensorData);
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
  
  // VOC control
  server.on("/voc/auto", handleVOCControl);
  
  server.begin();
}

void setupClockEndpoints() {
  server.on("/clock/relay", []() {
    if (server.hasArg("state")) {
      controlClockRelay(server.arg("state") == "on");
    }
    server.send(200, "text/plain", "OK");
  });
  
  server.on("/clock/brightness", HTTP_GET, []() {
    forwardClockRequest("/brightness/get");
  });
  
  server.on("/clock/brightness", HTTP_POST, []() {
    if (server.hasArg("value")) {
      setClockBrightness(server.arg("value").toInt());
    }
    server.send(200, "text/plain", "OK");
  });
  
  server.on("/clock/brightness/auto", HTTP_GET, []() {
    String response = httpGetString("http://" + clockIP + "/brightness/get");
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
      httpPost("http://" + clockIP + "/brightness/auto", "state=" + server.arg("state"));
      server.send(200, "text/plain", "OK");
    }
  });
  
  server.on("/clock/relay/status", HTTP_GET, []() {
    String response = httpGetString("http://" + clockIP + "/relay/state");
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
  
  switch (currentMode) {
    case SHOW_AVG_TEMP:     snprintf(buffer, 16, "%.1fC", avgTemp); break;
    case SHOW_AVG_HUMIDITY: snprintf(buffer, 16, "%.1f%%", avgHumid); break;
    case SHOW_HEAT_INDEX:   snprintf(buffer, 16, "HI:%.1fC", avgHeatIndex); break;
    case SHOW_PRESSURE:     snprintf(buffer, 16, "P:%.1f", displayedPressure); break;
    case SHOW_TVOC:         snprintf(buffer, 16, "%dPPB", displayedTVOC); break;
  }
  
  displayCenteredText(buffer);
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

void checkVOCLevels() {
  if (!autoVOCControl) return;
  
  bool shouldActivate = (remoteTVOC >= 1500 || avgHeatIndex >= 40);
  
  if (shouldActivate && !relayTriggeredByVOC) {
    controlClockRelay(true);
    relayTriggeredByVOC = true;
  } else if (!shouldActivate && relayTriggeredByVOC) {
    controlClockRelay(false);
    relayTriggeredByVOC = false;
  }
}

// ==================== SENSOR & DATA ====================
void handleSensorData() {
  if (DHT.read() == DHT20_OK) {
    sensorTemperature = DHT.getTemperature();
    sensorHumidity = DHT.getHumidity();
  }
  
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
  
  // Send JSON response
  String json = createSensorJSON();
  server.send(200, "application/json; charset=utf-8", json);
}

String getClockSensorData() {
  String response = httpGetString("http://" + clockIP + "/sensor/api");
  if (!response.isEmpty()) {
    DynamicJsonDocument doc(256);
    deserializeJson(doc, response);
    
    // Only update if values are valid
    if (doc.containsKey("temperature")) remoteTemp = doc["temperature"];
    if (doc.containsKey("humidity")) remoteHumid = doc["humidity"];
    if (doc.containsKey("pressure")) remotePressure = doc["pressure"];
    if (doc.containsKey("tvoc")) remoteTVOC = doc["tvoc"];
  }
  return response;
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
    millis() + (duration * 60000),
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
  String json = "{\"timers\":[";
  bool hasActive = false;
  
  for (int i = 0; i < 3; i++) {
    if (deviceTimers[i].active && deviceTimers[i].endTime > millis()) {
      if (hasActive) json += ",";
      
      unsigned long remaining = deviceTimers[i].endTime - millis();
      int minutes = remaining / 60000;
      int seconds = (remaining % 60000) / 1000;
      
      json += "{\"device\":\"" + getDeviceName(i) + "\","
              "\"deviceId\":\"" + getDeviceId(i) + "\","
              "\"targetState\":\"" + deviceTimers[i].targetState + "\","
              "\"remainingMinutes\":" + String(minutes) + ","
              "\"remainingSeconds\":" + String(seconds) + ","
              "\"remainingTime\":\"" + String(minutes) + "m " + String(seconds) + "s\"}";
      hasActive = true;
    }
  }
  
  json += "],\"hasActiveTimers\":" + String(hasActive ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleDeviceStates() {
  String json = "{\"fan\":\"" + outputs[0].state + "\","
                "\"bigLight\":\"" + outputs[1].state + "\","
                "\"light\":\"" + outputs[2].state + "\"}";
  server.send(200, "application/json", json);
}

void handleDeviceInfo() {
  server.send(200, "application/json", "{\"ip\":\"" + WiFi.localIP().toString() + "\"}");
}

void handleTimeAPI() {
  String json = "{\"epoch\":" + String(timeClient.getEpochTime()) + ","
                "\"hours\":" + String(timeClient.getHours()) + ","
                "\"minutes\":" + String(timeClient.getMinutes()) + ","
                "\"seconds\":" + String(timeClient.getSeconds()) + ","
                "\"formattedTime\":\"" + timeClient.getFormattedTime() + "\"}";
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
      controlClockRelay(false);
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

void controlClockRelay(bool state) {
  httpGet("http://" + clockIP + "/relay/" + (state ? "on" : "off"));
}

void setClockBrightness(int value) {
  httpPost("http://" + clockIP + "/brightness", "value=" + String(value));
}

void maintainWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED && millis() - lastReconnectTime >= 10000) {
    lastReconnectTime = millis();
    WiFi.reconnect();
  }
}

// ==================== HELPER FUNCTIONS ====================
String httpGetString(String url) {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  String result = (http.GET() == HTTP_CODE_OK) ? http.getString() : "";
  http.end();
  return result;
}

void httpGet(String url) {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.GET();
  http.end();
}

void httpPost(String url, String data) {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.POST(data);
  http.end();
}

void forwardClockRequest(String endpoint) {
  String response = httpGetString("http://" + clockIP + endpoint);
  server.send(response.isEmpty() ? 500 : 200, "application/json", 
              response.isEmpty() ? "{\"error\":\"failed\"}" : response);
}

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
  return "{\"avgTemp\":" + String(avgTemp, 1) + ","
         "\"avgHumid\":" + String(avgHumid, 1) + ","
         "\"heatIndex\":" + String(avgHeatIndex, 1) + ","
         "\"pressure\":" + String(displayedPressure, 1) + ","
         "\"tvoc\":" + String(displayedTVOC) + ","
         "\"brightness\":" + String(displayBrightness) + ","
         "\"autoBrightness\":" + String(autoBrightness ? "true" : "false") + "}";
}
