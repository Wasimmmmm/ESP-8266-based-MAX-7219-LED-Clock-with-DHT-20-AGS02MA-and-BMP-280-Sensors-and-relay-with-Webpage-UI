// In Arduino IDE go to Tools> MMU (It is after Debug Level)> Chnage to 16KB cache + 48KB IRAM(IRAM).
#include "credentials.h" // Either create a credentials.h file with the needed credentials or write the credentials directly into code between "".
#include "DHT20.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <digitalWriteFast.h> // https://docs.arduino.cc/libraries/digitalwritefast/

// Function Prototypes - for better resource management
void setupWiFiSensorsDisplay();
void setupWebServer();
void maintainWiFiConnection();
void checkTouchButton();
void updateDisplay();
void handleSensorData();
void checkTimer();
void checkVOCLevels();
void updateFanState(String state);
void controlClockRelay(bool state);
void setClockBrightness(int value);
String getClockSensorData();
void displayCenteredText(const char* text);
void setOledBrightness(uint8_t brightness);
String nextFanState(String currentState);
void handleTimer();
void handleTimerCancel();
void handleTimerStatus();
void toggleOutput(int index);
void handleDeviceStates();
void handleDeviceInfo();
void handleFanControl();
void handleAutoBrightness();
void handleBrightnessControl();
void handleReset();
void handleRoot();

DHT20 DHT;
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

enum DisplayMode {
  SHOW_AVG_TEMP,
  SHOW_AVG_HUMIDITY,
  SHOW_HEAT_INDEX,
  SHOW_PRESSURE,
  SHOW_TVOC
};
DisplayMode currentMode = SHOW_AVG_TEMP;

struct Output {
  uint8_t pin;
  String state;
  String name;
};

Output outputs[] = {
  { 16, "On", "Fan" },          // D0
  { 15, "Off", "Big Light" },   // D8
  { 12, "Off", "Light" },       // D6
  { 14, "On", "Low Speed" },    // D5 (Fan Low Speed)
};

const uint8_t touchPin = 13;  // D7

ESP8266WebServer server(80);
WiFiUDP udp;
NTPClient timeClient(udp, "time.google.com", 19800, 3600000);

unsigned long lastProcessTime = 0;
unsigned long lastSensorTime = 0;
unsigned long lastDisplayTime = 0;
unsigned long lastReconnectTime = 0;

//data from this device
float sensorTemperature = 0.0;
float sensorHumidity = 0.0;
float heatIndex = 0.0;
uint8_t displayBrightness = 60;
bool autoBrightness = true;
unsigned long lastBrightnessCheck = 0;

//data from second device on 192.168.0.110
String clockIP = CLOCK_IP;
float remoteTemp = 0.0;
float remoteHumid = 0.0;
float remotePressure = 0.0;
uint32_t remoteTVOC = 0;
float avgTemp = 0.0;
float avgHumid = 0.0;
float avgHeatIndex = 0.0;
float displayedPressure = 0.0;
uint32_t displayedTVOC = 0;
unsigned long currentBrightness = 0;

// For VOC auto relay control
bool autoVOCControl = true;
const uint32_t VOC_THRESHOLD = 1500;  // ppb threshold for turning on relay
bool relayTriggeredByVOC = false;

// Timer management structure
struct DeviceTimer {
  unsigned long endTime;
  String targetState;
  bool active;
};

// Array of timers (one for each controllable device)
DeviceTimer deviceTimers[3] = {
  { 0, "", false },  // Fan (index 0)
  { 0, "", false },  // Big Light (index 1)
  { 0, "", false }   // Light (index 2)
};

void setup() {
  Serial.begin(115200);
  setupWiFiSensorsDisplay();
  setupWebServer();
}

void loop() {
  unsigned long currentMillis = millis();

  server.handleClient();

  if (currentMillis - lastProcessTime > 50) {
    lastProcessTime = currentMillis;
    ArduinoOTA.handle();
    checkTouchButton();
    checkTimer();
  }

  if (currentMillis - lastDisplayTime > 2000) {
    lastDisplayTime = currentMillis;
    updateDisplay();
  }

  if (currentMillis - lastSensorTime > 10000) {
    lastSensorTime = currentMillis;
    handleSensorData();
    getClockSensorData();
    checkVOCLevels();
    maintainWiFiConnection();
  }

  if (autoBrightness && currentMillis - lastBrightnessCheck > 30000) {
    lastBrightnessCheck = currentMillis;
    int currentHour = timeClient.getHours();
    uint8_t newBrightness = (currentHour > 22 || currentHour < 7) ? 0 : 60;

    if (newBrightness != displayBrightness) {
      displayBrightness = newBrightness;
      setOledBrightness(displayBrightness);
    }
  }
  
  yield();
}

void setupWiFiSensorsDisplay(){
  WiFi.mode(WIFI_AP_STA);
  // Start AP for clock with SSID: JARVIS-AP, Password: your_password
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  // Connect to predefined STA network
  WiFi.begin(STA_SSID, STA_PASSWORD);
  
  Serial.println("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts++ < 30) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
  }

  // Setup OTA
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  Wire.begin();
  udp.begin(123);
  DHT.begin();

  // Initialize OLED
  u8g2.begin();
  displayCenteredText("JARVIS");
  delay(2000);

  // Initialize outputs
  pinMode(touchPin, INPUT_PULLUP);
  for (auto& output : outputs) {
    pinMode(output.pin, OUTPUT);
    if (output.name == "Fan") updateFanState(output.state);
    else digitalWriteFast(output.pin, (output.state == "On") ? HIGH : LOW);
  }

  timeClient.begin();
  timeClient.forceUpdate();
  handleSensorData();
  setOledBrightness((timeClient.getHours() < 7) ? 22 : 60);
}

void setupWebServer(){
  server.on("/", handleRoot);
  server.on("/reset", handleReset);
  server.on("/fan/toggle", handleFanControl);
  server.on("/output/1/toggle", []() {
    toggleOutput(1);
  });
  server.on("/output/2/toggle", []() {
    toggleOutput(2);
  });
  server.on("/sensorData", handleSensorData);
  server.on("/deviceStates", handleDeviceStates);
  server.on("/device/info", handleDeviceInfo);
  server.on("/brightness", handleBrightnessControl);
  server.on("/brightness/auto", handleAutoBrightness);
  server.on("/timer/status", handleTimerStatus);
  server.on("/timer", HTTP_POST, handleTimer);
  server.on("/timer/cancel", HTTP_POST, handleTimerCancel);
  server.on("/clock/relay", []() {
    if (server.hasArg("state")) {
      controlClockRelay(server.arg("state") == "on");
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/clock/brightness", HTTP_GET, []() {
    WiFiClient client;
    HTTPClient http;
    String url = "http://" + clockIP + "/brightness/get";
    http.begin(client, url);
    if (http.GET() == HTTP_CODE_OK) {
      String payload = http.getString();
      DynamicJsonDocument doc(256);
      deserializeJson(doc, payload);
      int brightness = doc["brightness"];
      bool autoState = doc["auto"];
      server.send(200, "application/json", payload); // Forward full JSON
    }
    http.end();
  });

  server.on("/clock/brightness", HTTP_POST, []() {
    if (server.hasArg("value")) {
      setClockBrightness(server.arg("value").toInt());
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/clock/brightness/auto", HTTP_GET, []() {
    WiFiClient client;
    HTTPClient http;
    String url = "http://" + clockIP + "/brightness/get";
    http.begin(client, url);
    if (http.GET() == HTTP_CODE_OK) {
      String payload = http.getString();
      DynamicJsonDocument doc(256);
      deserializeJson(doc, payload);
      bool autoState = doc["auto"];
      server.send(200, "text/plain", autoState ? "on" : "off");
    } else {
      server.send(500, "text/plain", "Error");
    }
    http.end();
  });

  server.on("/clock/brightness/auto", HTTP_POST, []() {
    if (server.hasArg("state")) {
      String state = server.arg("state");
      WiFiClient client;
      HTTPClient http;
      String url = "http://" + clockIP + "/brightness/auto";
      http.begin(client, url);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      String postData = "state=" + state;
      http.POST(postData);
      String response = http.getString();
      http.end();
      server.send(200, "text/plain", response);
    }
  });

  server.on("/clock/relay/status", HTTP_GET, []() {
    WiFiClient client;
    HTTPClient http;
    String url = "http://" + clockIP + "/relay/state";
    http.begin(client, url);
    if (http.GET() == HTTP_CODE_OK) {
      String response = http.getString();
      server.send(200, "text/plain", response);
    } else {
      server.send(500, "text/plain", "Error");
    }
    http.end();
  });

  server.on("/voc/auto", []() {
    if (server.hasArg("state")) {
      autoVOCControl = (server.arg("state") == "on");
      if (!autoVOCControl && relayTriggeredByVOC) {
        // Turn off relay control if feature is disabled
        controlClockRelay(false);
        relayTriggeredByVOC = false;
      }
    }
    server.send(200, "text/plain", autoVOCControl ? "on" : "off");
  });

  server.on("/api/time", HTTP_GET, []() {
    String timeJson = "{";
    timeJson += "\"epoch\":" + String(timeClient.getEpochTime()) + ",";
    timeJson += "\"hours\":" + String(timeClient.getHours()) + ",";
    timeJson += "\"minutes\":" + String(timeClient.getMinutes()) + ",";
    timeJson += "\"seconds\":" + String(timeClient.getSeconds()) + ",";
    timeJson += "\"formattedTime\":\"" + timeClient.getFormattedTime() + "\"";
    timeJson += "}";
    server.send(200, "application/json", timeJson);
  });

  server.begin();
}

void controlClockRelay(bool state) {
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + clockIP + "/relay/" + (state ? "on" : "off");
  http.begin(client, url);
  http.GET();
  http.end();
}

void setClockBrightness(int value) {
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + clockIP + "/brightness";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String postData = "value=" + String(value);
  http.POST(postData);
  http.end();
}

String getClockSensorData() {
  WiFiClient client;
  HTTPClient http;

  http.begin(client, "http://" + clockIP + "/sensor/api");  // Replace with your clock's IP

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(256);
    deserializeJson(doc, payload);

    remoteTemp = doc["temperature"];
    remoteHumid = doc["humidity"];
    remotePressure = doc["pressure"];
    remoteTVOC = doc["tvoc"];
  }
  http.end();
  return "{}";
}

void handleTimer() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  if (!server.hasArg("device") || !server.hasArg("state") || !server.hasArg("duration")) {
    server.send(400, "text/plain", "Missing parameters");
    return;
  }

  String device = server.arg("device");
  String state = server.arg("state");
  int duration = server.arg("duration").toInt();

  if (duration <= 0) {
    server.send(400, "text/plain", "Invalid duration");
    return;
  }

  // Set timer for the appropriate device
  int deviceIndex = -1;
  if (device == "fan") {
    deviceIndex = 0;
  } else if (device == "bigLight") {
    deviceIndex = 1;
  } else if (device == "light") {
    deviceIndex = 2;
  } else {
    server.send(400, "text/plain", "Invalid device");
    return;
  }

  // Set the timer for this device
  deviceTimers[deviceIndex].targetState = state;
  deviceTimers[deviceIndex].endTime = millis() + (duration * 60000);  // Convert minutes to milliseconds
  deviceTimers[deviceIndex].active = true;

  server.send(200, "text/plain", "Timer set for " + String(duration) + " minutes");
}

void handleTimerCancel() {
  if (server.hasArg("device")) {
    // Cancel specific device timer
    String device = server.arg("device");
    int deviceIndex = -1;

    if (device == "fan") deviceIndex = 0;
    else if (device == "bigLight") deviceIndex = 1;
    else if (device == "light") deviceIndex = 2;

    if (deviceIndex >= 0) {
      deviceTimers[deviceIndex].active = false;
      deviceTimers[deviceIndex].endTime = 0;
      deviceTimers[deviceIndex].targetState = "";
      server.send(200, "text/plain", "Timer for " + device + " cancelled");
    } else {
      server.send(400, "text/plain", "Invalid device");
    }
  } else {
    // Cancel all timers if no specific device is specified
    for (int i = 0; i < 3; i++) {
      deviceTimers[i].active = false;
      deviceTimers[i].endTime = 0;
      deviceTimers[i].targetState = "";
    }
    server.send(200, "text/plain", "All timers cancelled");
  }
}

void handleTimerStatus() {
  String json = "{\"timers\":[";
  bool hasActiveTimers = false;

  for (int i = 0; i < 3; i++) {
    if (deviceTimers[i].active && deviceTimers[i].endTime > millis()) {
      if (hasActiveTimers) json += ",";

      // Calculate remaining time
      unsigned long remainingMillis = deviceTimers[i].endTime - millis();
      int remainingMinutes = remainingMillis / 60000;
      int remainingSeconds = (remainingMillis % 60000) / 1000;

      // Get device name
      String deviceName;
      String deviceId;
      if (i == 0) {
        deviceName = "Fan";
        deviceId = "fan";
      } else if (i == 1) {
        deviceName = "Big Light";
        deviceId = "bigLight";
      } else if (i == 2) {
        deviceName = "Light";
        deviceId = "light";
      }

      json += "{";
      json += "\"device\":\"" + deviceName + "\",";
      json += "\"deviceId\":\"" + deviceId + "\",";
      json += "\"targetState\":\"" + deviceTimers[i].targetState + "\",";
      json += "\"remainingMinutes\":" + String(remainingMinutes) + ",";
      json += "\"remainingSeconds\":" + String(remainingSeconds) + ",";
      json += "\"remainingTime\":\"" + String(remainingMinutes) + "m " + String(remainingSeconds) + "s\"";
      json += "}";

      hasActiveTimers = true;
    }
  }

  json += "],\"hasActiveTimers\":" + String(hasActiveTimers ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void updateDisplay() {
    char buffer[16]; // Local buffer created each call

    switch (currentMode) {
      case SHOW_AVG_TEMP:
        snprintf(buffer, sizeof(buffer), "%2.1fC", avgTemp);
        break;
      case SHOW_AVG_HUMIDITY:
        snprintf(buffer, sizeof(buffer), "%2.1f%%", avgHumid);
        break;
      case SHOW_HEAT_INDEX:
        snprintf(buffer, sizeof(buffer), "HI:%2.1fC", avgHeatIndex);
        break;
      case SHOW_PRESSURE:
        snprintf(buffer, sizeof(buffer), "P:%2.1f", displayedPressure);
        break;
      case SHOW_TVOC:
        snprintf(buffer, sizeof(buffer), "%dPPB", displayedTVOC);
        break;
    }

    displayCenteredText(buffer);

    // Cycle to next display mode
    currentMode = static_cast<DisplayMode>((currentMode + 1) % (SHOW_TVOC + 1));
}

void checkTouchButton() {
  static bool lastState = HIGH;
  bool currentState = digitalReadFast(touchPin);
  if (currentState == HIGH && lastState == LOW) {
    // Using digitalWriteFast for toggle operations
    digitalWriteFast(outputs[1].pin, !digitalReadFast(outputs[1].pin));
    digitalWriteFast(outputs[2].pin, !digitalReadFast(outputs[2].pin));
    outputs[1].state = digitalReadFast(outputs[1].pin) ? "On" : "Off";
    outputs[2].state = digitalReadFast(outputs[2].pin) ? "On" : "Off";
  }
  lastState = currentState;
}

void setOledBrightness(uint8_t brightness) {
  displayBrightness = brightness;
  uint8_t contrast = map(brightness, 0, 100, 0, 255);
  u8g2.sendF("ca", 0x81, contrast);
}

String nextFanState(String currentState) {
  if (currentState == "Off") return "On";
  if (currentState == "On") return "Low Speed";
  return "Off";
}

void updateFanState(String state) {
  outputs[0].state = state;
  if (state == "Off") {
    digitalWriteFast(outputs[0].pin, LOW);
    digitalWriteFast(outputs[3].pin, HIGH);
  } else if (state == "On") {
    digitalWriteFast(outputs[0].pin, HIGH);
    digitalWriteFast(outputs[3].pin, HIGH);
  } else {
    digitalWriteFast(outputs[0].pin, LOW);
    digitalWriteFast(outputs[3].pin, LOW);
  }
}

void checkTimer() {
  unsigned long currentMillis = millis();

  for (int i = 0; i < 3; i++) {
    if (deviceTimers[i].active && deviceTimers[i].endTime > 0 && currentMillis >= deviceTimers[i].endTime) {
      // Timer expired, apply the state change
      if (i == 0) {
        // Fan
        updateFanState(deviceTimers[i].targetState);
      } else if (i >= 1 && i <= 2) {
        // Big Light or Light
        outputs[i].state = deviceTimers[i].targetState;
        digitalWriteFast(outputs[i].pin, (deviceTimers[i].targetState == "On") ? HIGH : LOW);
      }

      // Clear the timer
      deviceTimers[i].active = false;
      deviceTimers[i].endTime = 0;
      deviceTimers[i].targetState = "";
    }
  }
}

void handleSensorData() {
  if (DHT.read() == DHT20_OK) {
    sensorTemperature = DHT.getTemperature();
    sensorHumidity = DHT.getHumidity();
  }

  avgTemp = (sensorTemperature + remoteTemp) / 2.0;
  avgHumid = (sensorHumidity + remoteHumid) / 2.0;

  // Calculate heat index using NOAA formula
  float tempF = avgTemp * 9.0 / 5.0 + 32;
  float hiF = -42.379 + 2.04901523 * tempF
              + 10.14333127 * avgHumid
              - 0.22475541 * tempF * avgHumid
              - 0.00683783 * pow(tempF, 2)
              - 0.05481717 * pow(avgHumid, 2)
              + 0.00122874 * pow(tempF, 2) * avgHumid
              + 0.00085282 * tempF * pow(avgHumid, 2)
              - 0.00000199 * pow(tempF, 2) * pow(avgHumid, 2);
  avgHeatIndex = (hiF - 32) * 5.0 / 9.0;

  // Store remote values
  displayedPressure = remotePressure;
  displayedTVOC = remoteTVOC;

  // Create JSON response
  String json = "{";
  json += "\"avgTemp\":" + String(avgTemp, 1) + ",";
  json += "\"avgHumid\":" + String(avgHumid, 1) + ",";
  json += "\"heatIndex\":" + String(avgHeatIndex, 1) + ",";
  json += "\"pressure\":" + String(displayedPressure, 1) + ",";
  json += "\"tvoc\":" + String(displayedTVOC) + ",";
  json += "\"brightness\":" + String(displayBrightness) + ",";
  json += "\"autoBrightness\":" + String(autoBrightness ? "true" : "false");
  json += "}";

  server.send(200, "application/json; charset=utf-8", json);
}

void handleDeviceStates() {
  String json = "{";
  json += "\"fan\":\"" + outputs[0].state + "\",";
  json += "\"bigLight\":\"" + outputs[1].state + "\",";
  json += "\"light\":\"" + outputs[2].state + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleDeviceInfo() {
  server.send(200, "application/json", "{\"ip\":\"" + WiFi.localIP().toString() + "\"}");
}

void handleFanControl() {
  String newState = nextFanState(outputs[0].state);
  updateFanState(newState);
  server.send(200, "application/json", "{\"status\":\"success\"}");
}

void toggleOutput(int index) {
  if (index >= 1 && index <= 2) {
    outputs[index].state = (outputs[index].state == "On") ? "Off" : "On";
    digitalWriteFast(outputs[index].pin, (outputs[index].state == "On") ? HIGH : LOW);
    server.send(200, "application/json", "{\"status\":\"success\"}");
  }
}

void handleAutoBrightness() {
  if (server.hasArg("state")) {
    autoBrightness = (server.arg("state") == "true");
    if (autoBrightness) {
      int currentHour = timeClient.getHours();
      displayBrightness = (currentHour > 22 || currentHour < 7) ? 0 : 60;
      setOledBrightness(displayBrightness);
    }
    server.send(200, "application/json", "{\"auto\":" + String(autoBrightness ? "true" : "false") + "}");
  }
}

void handleBrightnessControl() {
  if (server.hasArg("value")) {
    autoBrightness = false;  // Disable auto when manual adjustment
    int value = server.arg("value").toInt();
    value = constrain(value, 0, 100);
    setOledBrightness(value);
    server.send(200, "text/plain", String(value));
  }
}

void handleReset() {
  server.send(200, "text/plain", "Restarting...");
  delay(1000);
  ESP.restart();
}

void displayCenteredText(const char* text) {
  u8g2.setFont(u8g2_font_helvB24_tr);
  int textWidth = u8g2.getStrWidth(text);
  u8g2.clearBuffer();
  int x = (128 - textWidth) / 2;
  int y = (32 + u8g2.getMaxCharHeight()) / 2;
  u8g2.drawStr(x, y, text);
  u8g2.sendBuffer();
}

void checkVOCLevels() {
  if (autoVOCControl) {
    if (remoteTVOC > VOC_THRESHOLD || avgTemp >= 35) {
      // Turn on relay if VOC is high or temp is high
      if (!relayTriggeredByVOC) {
        controlClockRelay(true);
        relayTriggeredByVOC = true;
      }
    } else if (relayTriggeredByVOC && remoteTVOC <= VOC_THRESHOLD && avgTemp < 35) {
      // Turn off relay if both VOC and temp are below thresholds
      controlClockRelay(false);
      relayTriggeredByVOC = false;
    }
  }
}

void maintainWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long currentMillis = millis();
    if (currentMillis - lastReconnectTime >= 10000) {  // Attempt reconnect every 10 seconds
      lastReconnectTime = currentMillis;
      WiFi.reconnect();
    }
  }
}

const char* MAIN_page = R"=====(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>JARVIS Smart Home</title>
  <style>
    :root {
      --primary: #2196F3;
      --success: #4CAF50;
      --danger: #f44336;
      --light: #f8f9fa;
      --dark: #343a40;
      --gray: #6c757d;
      --border-radius: 12px;
    }
    
    * {
      box-sizing: border-box;
      font-family: 'Segoe UI', Roboto, Arial, sans-serif;
    }
    
    body {
      margin: 0;
      padding: 0;
      background-color: #f0f0f0;
      color: #333;
    }
    
    .container {
      max-width: 600px;
      margin: 0 auto;
      padding: 20px;
    }
    
    header {
      text-align: center;
      margin-bottom: 20px;
      background: white;
      border-radius: var(--border-radius);
      padding: 15px;
      box-shadow: 0 2px 10px rgba(0,0,0,0.1);
    }
    
    h1 {
      margin: 0;
      color: var(--primary);
      font-size: 1.8rem;
    }
    
    .card {
      background: white;
      border-radius: var(--border-radius);
      margin-bottom: 20px;
      overflow: hidden;
      box-shadow: 0 2px 10px rgba(0,0,0,0.1);
    }
    
    .card-header {
      background: var(--primary);
      color: white;
      padding: 12px 15px;
      font-weight: 600;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    
    .card-body {
      padding: 15px;
    }
    
    /* Environment Grid */
    .sensor-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 10px;
    }
    
    .sensor-item {
      background: var(--light);
      padding: 15px 10px;
      border-radius: 8px;
      text-align: center;
    }
    
    .sensor-label {
      font-size: 0.85rem;
      color: var(--gray);
      margin-bottom: 5px;
    }
    
    .sensor-value {
      font-size: 1.2rem;
      font-weight: 600;
      color: var(--dark);
    }
    
    /* Controls */
    .control-item {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 12px 0;
      border-bottom: 1px solid #eee;
    }
    
    .control-item:last-child {
      border-bottom: none;
    }
    
    .control-label {
      display: flex;
      align-items: center;
      gap: 10px;
      font-weight: 500;
    }
    
    .control-status {
      font-size: 0.8rem;
      color: var(--gray);
      margin-top: 4px;
    }
    
    /* Switch Toggle */
    .switch {
      position: relative;
      display: inline-block;
      width: 50px;
      height: 26px;
    }
    
    .switch input {
      opacity: 0;
      width: 0;
      height: 0;
    }
    
    .slider {
      position: absolute;
      cursor: pointer;
      top: 0;
      left: 0;
      right: 0;
      bottom: 0;
      background-color: #ccc;
      transition: .4s;
      border-radius: 34px;
    }
    
    .slider:before {
      position: absolute;
      content: "";
      height: 20px;
      width: 20px;
      left: 3px;
      bottom: 3px;
      background-color: white;
      transition: .4s;
      border-radius: 50%;
    }
    
    input:checked + .slider {
      background-color: var(--primary);
    }
    
    input:checked + .slider:before {
      transform: translateX(24px);
    }
    
    /* Range Slider */
    .range-control {
      width: 100%;
      margin: 15px 0;
    }
    
    .range-slider {
      -webkit-appearance: none;
      width: 100%;
      height: 8px;
      border-radius: 5px;
      background: #d3d3d3;
      outline: none;
    }
    
    .range-slider::-webkit-slider-thumb {
      -webkit-appearance: none;
      appearance: none;
      width: 20px;
      height: 20px;
      border-radius: 50%;
      background: var(--primary);
      cursor: pointer;
    }
    
    /* Timer section */
    .timer-controls {
      display: grid;
      grid-template-columns: 1fr 1fr 1fr;
      gap: 10px;
      margin-bottom: 15px;
    }
    
    .timer-select, .timer-input {
      padding: 10px;
      border: 1px solid #ddd;
      border-radius: 6px;
      font-size: 0.9rem;
    }
    
    .timer-actions {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
    }
    
    .btn {
      padding: 10px;
      border: none;
      border-radius: 6px;
      cursor: pointer;
      font-weight: 500;
      color: white;
      transition: opacity 0.3s;
    }
    
    .btn-primary {
      background-color: var(--primary);
    }
    
    .btn-success {
      background-color: var(--success);
    }
    
    .btn-danger {
      background-color: var(--danger);
    }
    
    .btn:disabled {
      opacity: 0.6;
      cursor: not-allowed;
    }
    
    .timer-status {
      padding: 10px;
      background: #f8f9fa;
      border-radius: 6px;
      margin-top: 10px;
      font-size: 0.9rem;
    }
    
    .timer-item {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 5px 0;
    }
    
    .timer-cancel-btn {
      background: var(--danger);
      color: white;
      border: none;
      border-radius: 50%;
      width: 20px;
      height: 20px;
      line-height: 1;
      cursor: pointer;
    }
    
    .footer {
      text-align: center;
      margin-top: 20px;
      padding: 10px;
      color: var(--gray);
      font-size: 0.8rem;
    }
    
    #loadingOverlay {
      position: fixed;
      top: 0; left: 0;
      width: 100%; height: 100%;
      background: rgba(255,255,255,0.9);
      display: flex;
      align-items: center;
      justify-content: center;
      z-index: 9999;
      transition: opacity 0.5s ease;
    }
    
    #loadingContent {
      font-size: 1.5rem;
      user-select: none;
    }

    @media (max-width: 480px) {
      .sensor-grid {
        grid-template-columns: repeat(2, 1fr);
      }
    }
  </style>
</head>
<body>
  <div id="loadingOverlay">
    <div id="loadingContent">
      <span style="color:#4285F4">L</span>
      <span style="color:#DB4437">o</span>
      <span style="color:#F4B400">a</span>
      <span style="color:#4285F4">d</span>
      <span style="color:#0F9D58">i</span>
      <span style="color:#DB4437">n</span>
      <span style="color:#F4B400">g</span>
      <span>&nbsp;</span>
      <span id="loadingPercent">0%</span>
    </div>
  </div>

  <div class="container">
    <header>
      <h1>🏠 JARVIS Smart Home</h1>
      <div id="ipAddress" style="font-size: 0.8rem; color: #666; margin-top: 5px;"></div>
    </header>

    <!-- Environment Card -->
    <div class="card">
      <div class="card-header">Environment Sensors</div>
      <div class="card-body">
        <div class="sensor-grid">
          <div class="sensor-item">
            <div class="sensor-label">🌡️ Temperature</div>
            <div class="sensor-value" id="avgTemp">--°C</div>
          </div>
          <div class="sensor-item">
            <div class="sensor-label">💧 Humidity</div>
            <div class="sensor-value" id="avgHumid">--%</div>
          </div>
          <div class="sensor-item">
            <div class="sensor-label">🔥 Feels Like</div>
            <div class="sensor-value" id="heatIndex">--°C</div>
          </div>
          <div class="sensor-item">
            <div class="sensor-label">📡 Pressure</div>
            <div class="sensor-value" id="pressure">--mmHg</div>
          </div>
          <div class="sensor-item">
            <div class="sensor-label">🌫️ TVOC</div>
            <div class="sensor-value" id="tvoc">--ppb</div>
          </div>
          <div class="sensor-item">
            <div class="sensor-label">💻 Display</div>
            <div class="sensor-value" id="brightnessValue">--%</div>
          </div>
        </div>
      </div>
    </div>

    <!-- Device Controls Card -->
    <div class="card">
      <div class="card-header">Device Controls</div>
      <div class="card-body">
        <div class="control-item">
          <div>
            <div class="control-label">🌀 Fan</div>
            <div class="control-status" id="fanStatus">--</div>
          </div>
          <label class="switch">
            <input type="checkbox" id="fanToggle" onclick="toggleDevice('fan')">
            <span class="slider"></span>
          </label>
        </div>
        
        <div class="control-item">
          <div>
            <div class="control-label">💡 Big Light</div>
            <div class="control-status" id="bigLightStatus">--</div>
          </div>
          <label class="switch">
            <input type="checkbox" id="bigLightToggle" onclick="toggleDevice('bigLight')">
            <span class="slider"></span>
          </label>
        </div>
        
        <div class="control-item">
          <div>
            <div class="control-label">🔆 Light</div>
            <div class="control-status" id="lightStatus">--</div>
          </div>
          <label class="switch">
            <input type="checkbox" id="lightToggle" onclick="toggleDevice('light')">
            <span class="slider"></span>
          </label>
        </div>
        
        <div class="control-item">
          <div>
            <div class="control-label">🌿 Auto VOC Control</div>
            <div class="control-status">Activates exhaust fan at 1500ppb or 35°C</div>
          </div>
          <label class="switch">
            <input type="checkbox" id="vocAutoToggle" onclick="toggleVOCControl(this.checked)">
            <span class="slider"></span>
          </label>
        </div>
        
        <div class="control-item">
          <div>
            <div class="control-label">⏰ Clock Relay</div>
            <div class="control-status">Remote device control</div>
          </div>
          <label class="switch">
            <input type="checkbox" id="clockRelay" onclick="toggleClockRelay()">
            <span class="slider"></span>
          </label>
        </div>
      </div>
    </div>

    <!-- Timer Card -->
    <div class="card">
      <div class="card-header">Timer Controls</div>
      <div class="card-body">
        <div class="timer-controls">
          <select id="timerDevice" class="timer-select">
            <option value="fan">Fan</option>
            <option value="bigLight">Big Light</option>
            <option value="light">Light</option>
          </select>
          <select id="timerState" class="timer-select"></select>
          <input type="number" id="timerDuration" class="timer-input" placeholder="Minutes" min="1">
        </div>
        
        <div class="timer-actions">
          <button onclick="startTimer()" class="btn btn-success">Start Timer</button>
          <button onclick="cancelTimer()" class="btn btn-danger">Cancel All</button>
        </div>
        
        <div id="timerStatus" class="timer-status">No active timers</div>
      </div>
    </div>

    <!-- Settings Card -->
    <div class="card">
      <div class="card-header">Settings</div>
      <div class="card-body">
        <div class="control-item">
          <div class="control-label">🤖 Auto Brightness</div>
          <label class="switch">
            <input type="checkbox" id="autoBrightnessToggle" onchange="toggleAutoBrightness(this.checked)">
            <span class="slider"></span>
          </label>
        </div>
        
        <div class="range-control">
          <label>🔆 Display Brightness</label>
          <input type="range" min="0" max="100" class="range-slider" id="brightnessSlider" 
                 oninput="updateBrightness(this.value)" onchange="setBrightness()">
        </div>

        <div class="control-item">
          <div class="control-label">🤖 Clock Auto Brightness</div>
          <label class="switch">
            <input type="checkbox" id="clockAutoBrightnessToggle" 
                  onchange="toggleClockAutoBrightness(this.checked)">
            <span class="slider"></span>
          </label>
        </div>
        
        <div class="range-control">
          <label>⏰ Clock Brightness</label>
          <div style="display: flex; align-items: center; gap: 10px;">
            <input type="range" min="0" max="15" value="5" class="range-slider" 
                  id="clockBrightnessSlider" oninput="updateClockBrightness(this.value)" disabled>
            
            <span id="clockBrightness">5</span>
          </div>
        </div>
        
        <div style="text-align: center; margin-top: 15px;">
          <button class="btn btn-danger" onclick="restartDevice()">🔄 Restart Device</button>
        </div>
      </div>
    </div>

    <div class="footer">
      JARVIS Smart Home Control System
    </div>
  </div>

  <script>
    // Loading animation
    const overlay = document.getElementById('loadingOverlay');
    const pctEl = document.getElementById('loadingPercent');
    let pct = 0;

    const fake = setInterval(() => {
      if (pct < 90) pctEl.textContent = (++pct) + '%';
      else clearInterval(fake);
    }, 50);

    function hideOverlay() {
      clearInterval(fake);
      pctEl.textContent = '100%';
      overlay.style.opacity = '0';
      overlay.addEventListener('transitionend', () => overlay.remove());
    }

    // Initial data loading
    function fetchSensors() {
      return fetch('/sensorData')
        .then(r => r.json())
        .then(data => {
          document.getElementById('avgTemp').textContent = data.avgTemp + '°C';
          document.getElementById('avgHumid').textContent = data.avgHumid + '%';
          document.getElementById('heatIndex').textContent = data.heatIndex + '°C';
          document.getElementById('pressure').textContent = data.pressure + 'mmHg';
          document.getElementById('tvoc').textContent = data.tvoc + 'ppb';
          document.getElementById('brightnessValue').textContent = data.brightness + '%';
          document.getElementById('brightnessSlider').value = data.brightness;
          document.getElementById('autoBrightnessToggle').checked = data.autoBrightness;
          document.getElementById('brightnessSlider').disabled = data.autoBrightness;
        });
    }


    function fetchDevices() {
      return fetch('/deviceStates')
        .then(r => r.json())
        .then(data => {
          document.getElementById('fanStatus').textContent = data.fan;
          document.getElementById('fanToggle').checked = !data.fan.includes('Off');
          document.getElementById('bigLightStatus').textContent = data.bigLight;
          document.getElementById('bigLightToggle').checked = data.bigLight === 'On';
          document.getElementById('lightStatus').textContent = data.light;
          document.getElementById('lightToggle').checked = data.light === 'On';
        });
    }

    function fetchTimer() {
      return fetch('/timer/status')
        .then(r => r.json())
        .then(data => {
          const el = document.getElementById('timerStatus');
          if (!data.hasActiveTimers) {
            el.textContent = 'No active timers';
            el.style.color = '#666';
          } else {
            el.innerHTML = data.timers.map(t =>
              `<div class="timer-item">${t.device} → <b>${t.targetState}</b> in <b>${t.remainingTime}</b>
               <button class="timer-cancel-btn" onclick="cancelSpecificTimer('${t.deviceId}')">×</button></div>`
            ).join('');
            el.style.color = '#2196F3';
          }
        });
    }

    function fetchInfo() {
      return fetch('/device/info')
        .then(r => r.json())
        .then(data => {
          document.getElementById('ipAddress').textContent = 'IP: ' + data.ip;
        });
    }

    // Timer state options
    function updateTimerStateOptions() {
      const device = document.getElementById('timerDevice').value;
      const stateSelect = document.getElementById('timerState');
      stateSelect.innerHTML = '';
      
      const states = device === 'fan' 
        ? ['Off', 'On', 'Low Speed'] 
        : ['On', 'Off'];
        
      states.forEach(state => {
        const option = document.createElement('option');
        option.value = state;
        option.textContent = state;
        stateSelect.appendChild(option);
      });
    }

    // Device control functions
    function toggleDevice(device) {
      const endpoints = {
        fan: '/fan/toggle',
        bigLight: '/output/1/toggle',
        light: '/output/2/toggle'
      };
      fetch(endpoints[device])
        .then(() => fetchDevices());
    }

    function toggleAutoBrightness(state) {
      fetch(`/brightness/auto?state=${state ? 'on' : 'off'}`)
        .then(() => {
          document.getElementById('brightnessSlider').disabled = state;
          fetchSensors();
        });
    }

    function updateBrightness(value) {
      document.getElementById('brightnessValue').textContent = value + '%';
    }

    function setBrightness() {
      const value = document.getElementById('brightnessSlider').value;
      fetch(`/brightness?value=${value}`);
    }

    function toggleClockRelay() {
      const isChecked = document.getElementById('clockRelay').checked;
      fetch('/clock/relay?state=' + (isChecked ? 'on' : 'off'));
    }

    function updateClockBrightness(value) {
      document.getElementById('clockBrightness').textContent = value;
      fetch('/clock/brightness?value=' + value);
    }

    function toggleVOCControl(state) {
      fetch('/voc/auto?state=' + (state ? 'on' : 'off'));
    }

    // Timer functions
    function startTimer() {
      const device = document.getElementById('timerDevice').value;
      const state = document.getElementById('timerState').value;
      const duration = document.getElementById('timerDuration').value;

      if (!duration || duration < 1) {
        alert("Please enter a valid duration in minutes");
        return;
      }

      fetch('/timer', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: `device=${device}&state=${state}&duration=${duration}`
      })
      .then(response => response.text())
      .then(data => {
        alert(data);
        fetchTimer();
      });
    }

    function cancelTimer() {
      fetch('/timer/cancel', { method: 'POST' })
      .then(response => response.text())
      .then(data => {
        alert(data);
        fetchTimer();
      });
    }

    function cancelSpecificTimer(device) {
      fetch('/timer/cancel?device=' + device, { method: 'POST' })
        .then(() => fetchTimer());
    }

    function restartDevice() {
      if (confirm("Restart JARVIS?")) {
        fetch('/reset')
          .then(() => {
            alert("Restarting device. Page will reload in 5 seconds.");
            setTimeout(() => location.reload(), 5000);
          });
      }
    }

    // Initialize the page
    document.addEventListener('DOMContentLoaded', () => {
      updateTimerStateOptions();
      document.getElementById('timerDevice').addEventListener('change', updateTimerStateOptions);
      
      // Get initial VOC control state
      fetch('/voc/auto')
        .then(r => r.text())
        .then(state => {
          document.getElementById('vocAutoToggle').checked = (state === 'on');
        });

      // In fetchInfo() or initialization
      fetch('/clock/relay/status')
        .then(r => r.text())
        .then(state => {
          document.getElementById('clockRelay').checked = (state === 'ON');
        });

      // Get initial clock relay state and brightness
      fetch('/clock/brightness')
        .then(r => r.json())
        .then(data => {
          document.getElementById('clockBrightnessSlider').value = data.brightness;
          document.getElementById('clockBrightness').textContent = data.brightness;
          document.getElementById('clockAutoBrightnessToggle').checked = data.auto;
        });

      fetch('/clock/brightness/auto')
        .then(r => r.text())
        .then(state => {
          document.getElementById('clockAutoBrightnessToggle').checked = (state === 'on');
          document.getElementById('clockBrightnessSlider').disabled = (state === 'on');
        });
      
      // Load all initial data
      Promise.all([
        fetchSensors(),
        fetchDevices(),
        fetchTimer(),
        fetchInfo()
      ])
      .then(hideOverlay)
      .catch(err => {
        console.error('Initial data load failed', err);
        hideOverlay();
      })
      .finally(() => {
        // Start regular updates
        setInterval(fetchSensors, 2000);
        setInterval(fetchDevices, 2000);
        setInterval(fetchTimer, 2000);
      });
    });
  </script>
</body>
</html>
)=====";

void handleRoot() {
  server.send(200, "text/html", MAIN_page);
}
