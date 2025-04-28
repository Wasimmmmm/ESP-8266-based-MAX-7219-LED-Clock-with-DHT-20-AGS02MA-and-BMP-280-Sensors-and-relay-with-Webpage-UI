#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <NTPClient.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <Wire.h>
#include <DHT20.h>
#include <AGS02MA.h>
#include <Adafruit_BMP280.h>

// WiFi credentials
const char* ssid = "2.4G";
const char* password = "wifipassword";

// OTA settings
const char* hostname = "ESP-Clock"; // Device hostname for OTA identification

// Define hardware type for MAX7219
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4      // 4 modules of 8x8 LEDs
#define CS_PIN D8          // CS pin connected to D8

// Define relay pin
#define RELAY_PIN D3       // Connect relay to D3 pin

// Define brightness levels
#define NIGHT_BRIGHTNESS 0
#define DAY_BRIGHTNESS 5
#define MIN_BRIGHTNESS 0
#define MAX_BRIGHTNESS 15

// Create a new instance of the MD_Parola class for scrolling text
MD_Parola P = MD_Parola(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

// Set up ESP8266 web server
ESP8266WebServer server(80);

// Set up NTP client
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

// Sensor objects
DHT20 dht20;
AGS02MA ags;
Adafruit_BMP280 bmp; // BMP280 sensor

// Variables for sensor readings
float temperature = 0.0;
float humidity = 0.0;
float heatIndex = 0.0;
uint32_t tvocPPB = 0;
float pressure = 0.0;
unsigned long lastSensorUpdate = 0;
const long sensorUpdateInterval = 10000; // Update sensor readings every 10 seconds

// Variables to store time components
int hours, minutes, seconds;
char timeString[6]; // To store the formatted time string (HH:MM)
bool relayState = false; // Track relay state
int currentBrightness = DAY_BRIGHTNESS; // Track current brightness
bool autoBrightness = true; // Track if auto brightness is enabled
unsigned long lastReconnectTime = 0;

// Variables for blinking colon
bool colonVisible = true;
unsigned long previousMillis = 0;
const long blinkInterval = 1000; // Blink every 1000ms (1 second)

// Simple HTML page with toggle switch, brightness slider, and sensor data
const char* MAIN_page = R"=====(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="UTF-8">
  <title>ESP8266 Display Control</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      text-align: center;
      margin: 20px;
    }
    h1 {
      color: #0066cc;
    }
    .container {
      max-width: 350px;
      margin: 0 auto;
      padding: 20px;
      border: 1px solid #ddd;
      border-radius: 10px;
      background-color: #f9f9f9;
    }
    .section {
      margin-bottom: 30px;
      padding-bottom: 20px;
      border-bottom: 1px solid #eee;
    }
    .switch {
      position: relative;
      display: inline-block;
      width: 60px;
      height: 34px;
      margin: 20px 0;
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
      height: 26px;
      width: 26px;
      left: 4px;
      bottom: 4px;
      background-color: white;
      transition: .4s;
      border-radius: 50%;
    }
    input:checked + .slider {
      background-color: #2196F3;
    }
    input:checked + .slider:before {
      transform: translateX(26px);
    }
    .status {
      margin-top: 10px;
      font-weight: bold;
    }
    .brightness-control {
      margin: 20px 0;
      text-align: center;
    }
    .brightness-slider {
      width: 80%;
      margin: 10px auto;
    }
    .checkbox-control {
      margin: 10px 0;
    }
    .footer {
      margin-top: 20px;
      font-size: 12px;
      color: #666;
    }
    .restart-button {
      background-color: #ff4545;
      color: white;
      border: none;
      padding: 10px 20px;
      text-align: center;
      text-decoration: none;
      display: inline-block;
      font-size: 16px;
      margin: 10px 0;
      cursor: pointer;
      border-radius: 5px;
      transition: background-color 0.3s;
    }
    .restart-button:hover {
      background-color: #cc3636;
    }
    .sensor-data {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
      margin-top: 15px;
    }
    .sensor-card {
      background-color: #fff;
      padding: 15px;
      border-radius: 8px;
      box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }
    .sensor-value {
      font-size: 24px;
      font-weight: bold;
      color: #0066cc;
      margin: 5px 0;
    }
    .sensor-unit {
      font-size: 14px;
      color: #666;
    }
    .refresh-button {
      background-color: #4CAF50;
      color: white;
      border: none;
      padding: 8px 15px;
      text-align: center;
      text-decoration: none;
      display: inline-block;
      font-size: 14px;
      margin: 10px 0;
      cursor: pointer;
      border-radius: 5px;
      transition: background-color 0.3s;
    }
    .refresh-button:hover {
      background-color: #45a049;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>ESP8266 Control Panel</h1>

    <div class="section">
      <h2>Sensor Data</h2>
      <div class="sensor-data">
        <div class="sensor-card">
          <div>Temperature</div>
          <div class="sensor-value">
            <span id="temp-value">--</span><span class="sensor-unit"> °C</span>
          </div>
        </div>

        <div class="sensor-card">
          <div>Humidity</div>
          <div class="sensor-value">
            <span id="humid-value">--</span><span class="sensor-unit"> %</span>
          </div>
        </div>

        <div class="sensor-card">
          <div>Heat Index</div>
          <div class="sensor-value">
            <span id="heat-value">--</span><span class="sensor-unit"> °C</span>
          </div>
        </div>

        <div class="sensor-card">
          <div>TVOC</div>
          <div class="sensor-value">
            <span id="tvoc-value">--</span><span class="sensor-unit"> ppb</span>
          </div>
        </div>

        <div class="sensor-card">
          <div>Pressure</div>
          <div class="sensor-value">
            <span id="pressure-value">--</span><span class="sensor-unit"> mmHg</span>
          </div>
        </div>
      <button class="refresh-button" onclick="getSensorData()">Refresh Sensor Data</button>
    </div>

    <div class="section">
      <h2>Display Brightness</h2>
      <div class="checkbox-control">
        <input type="checkbox" id="autoBrightnessToggle" checked onclick="toggleAutoBrightness()">
        <label for="autoBrightnessToggle">Auto Brightness (based on time)</label>
      </div>
      <div class="brightness-control">
        <label for="brightnessSlider">Brightness: <span id="brightnessValue">5</span></label>
        <input type="range" min="0" max="15" value="5" class="brightness-slider" id="brightnessSlider" 
               oninput="updateBrightnessValue()" onchange="setBrightness()">
      </div>
    </div>
    
    <div class="section">
      <h2>Relay Control</h2>
      <label class="switch">
        <input type="checkbox" id="relayToggle" onclick="toggleRelay()">
        <span class="slider"></span>
      </label>
      <div class="status" id="relayStatus">Relay is OFF</div>
    </div>

    <div class="section">
      <h2>Device Control</h2>
      <button class="restart-button" onclick="restartDevice()">Restart Device</button>
      <div class="status" id="restartStatus"></div>
    </div>
    
    <div class="footer">
      <p>Device Name: <span id="deviceName">ESP-Clock</span></p>
      <p>IP Address: <span id="ipAddress">loading...</span></p>
    </div>
  </div>
  
  <script>
    // Get sensor data
    function getSensorData() {
      fetch('/sensor/data')
        .then(response => response.json())
        .then(data => {
          document.getElementById('temp-value').innerText = data.temperature;
          document.getElementById('humid-value').innerText = data.humidity;
          document.getElementById('heat-value').innerText = data.heatIndex;
          document.getElementById('tvoc-value').innerText = data.tvoc;
          document.getElementById('pressure-value').innerText = data.pressure;
        });
    }
    
    // Update relay status
    function updateRelayStatus() {
      fetch('/relay/status')
        .then(response => response.text())
        .then(data => {
          document.getElementById('relayStatus').innerText = 'Relay is ' + data;
          document.getElementById('relayToggle').checked = (data === 'ON');
        });
    }
    
    // Toggle relay
    function toggleRelay() {
      var isChecked = document.getElementById('relayToggle').checked;
      fetch('/relay/' + (isChecked ? 'on' : 'off'))
        .then(response => response.text())
        .then(data => {
          document.getElementById('relayStatus').innerText = 'Relay is ' + data;
        });
    }
    
    // Update brightness display value
    function updateBrightnessValue() {
      var brightnessValue = document.getElementById('brightnessSlider').value;
      document.getElementById('brightnessValue').innerText = brightnessValue;
    }
    
    // Set brightness
    function setBrightness() {
      var brightnessValue = document.getElementById('brightnessSlider').value;
      fetch('/brightness/set?value=' + brightnessValue)
        .then(response => response.text())
        .then(data => {
          console.log("Brightness set to: " + data);
        });
    }
    
    // Toggle auto brightness
    function toggleAutoBrightness() {
      var isChecked = document.getElementById('autoBrightnessToggle').checked;
      fetch('/brightness/auto?state=' + (isChecked ? 'on' : 'off'))
        .then(response => response.text())
        .then(data => {
          // If auto brightness is off, enable the slider, otherwise disable it
          document.getElementById('brightnessSlider').disabled = isChecked;
          // Update the current brightness value from the response
          document.getElementById('brightnessValue').innerText = data;
          document.getElementById('brightnessSlider').value = data;
        });
    }
    
    // Get initial brightness
    function getBrightness() {
      fetch('/brightness/get')
        .then(response => response.json())
        .then(data => {
          document.getElementById('brightnessValue').innerText = data.brightness;
          document.getElementById('brightnessSlider').value = data.brightness;
          document.getElementById('autoBrightnessToggle').checked = data.auto;
          document.getElementById('brightnessSlider').disabled = data.auto;
        });
    }

    // Restart device
    function restartDevice() {
      if (confirm("Are you sure you want to restart the device?")) {
        document.getElementById('restartStatus').innerText = "Restarting...";
        fetch('/device/restart')
          .then(response => response.text())
          .then(data => {
            document.getElementById('restartStatus').innerText = data;
            // Set timeout to update the status after device has restarted
            setTimeout(function() {
              document.getElementById('restartStatus').innerText = "Reconnecting...";
              // Try to reconnect after estimated restart time
              setTimeout(checkConnection, 15000);
            }, 2000);
          })
          .catch(error => {
            document.getElementById('restartStatus').innerText = "Error: " + error;
          });
      }
    }

    // Check if device is back online
    function checkConnection() {
      fetch('/device/info')
        .then(response => response.json())
        .then(data => {
          document.getElementById('restartStatus').innerText = "Restart complete!";
          // Refresh all data
          updateRelayStatus();
          getBrightness();
          getDeviceInfo();
          getSensorData();
        })
        .catch(error => {
          document.getElementById('restartStatus').innerText = "Still restarting... retrying";
          setTimeout(checkConnection, 5000);
        });
    }
    
    // Get device info
    function getDeviceInfo() {
      fetch('/device/info')
        .then(response => response.json())
        .then(data => {
          document.getElementById('deviceName').innerText = data.hostname;
          document.getElementById('ipAddress').innerText = data.ip;
        });
    }
    
    // Initial updates
    updateRelayStatus();
    getBrightness();
    getDeviceInfo();
    getSensorData();
    
    // Periodic updates
    setInterval(getSensorData, 30000); // Update sensor data every 30 seconds
  </script>
</body>
</html>
)=====";

void setup() {
  Serial.begin(115200);
  
  // Initialize the relay pin as output and turn it off
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  
  // Initialize I2C for sensors
  Wire.begin(D2, D1); // SDA=D2, SCL=D1
  Wire.setClock(30000); // Set to 30 KHz for AGS02MA compatibility
  
  // Initialize sensors
  Serial.println("Initializing sensors...");
  dht20.begin();
  bmp.begin(0x76); 
  
  
  if (!ags.begin()) {
    Serial.println("Failed to initialize AGS02MA!");
    // Continue anyway in case the sensor is not connected
  } else {
    ags.setI2CResetSpeed(100000); // Reset to 100KHz after operations
  }
  
  // Initialize the MAX7219 display
  P.begin();
  P.setIntensity(DAY_BRIGHTNESS); // Default brightness
  P.displayClear();
  P.setTextAlignment(PA_CENTER); // Center the text
  
  // Connect to WiFi
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  // Set up OTA
  setupOTA();
  
  // Initialize NTP client
  timeClient.begin();
  timeClient.setTimeOffset(19800); // Set your timezone offset in seconds (e.g., GMT+5:30 = 19800)
  
  // Set up web server routes
  server.on("/", handleRoot);
  server.on("/relay/on", handleRelayOn);
  server.on("/relay/off", handleRelayOff);
  server.on("/relay/status", handleRelayStatus);
  server.on("/brightness/set", handleSetBrightness);
  server.on("/brightness/get", handleGetBrightness);
  server.on("/brightness/auto", handleAutoBrightness);
  server.on("/device/info", handleDeviceInfo);
  server.on("/device/restart", handleRestart);
  server.on("/sensor/data", handleSensorData);
  
  // Start the server
  server.begin();
  Serial.println("HTTP server started");
  
  // Initialize time string with colon visible
  updateTimeDisplay();
  
  // Initial sensor readings
  updateSensorReadings();
}

// New function to update the time display with or without colon based on blinking state
void updateTimeDisplay() {
  if (colonVisible) {
    sprintf(timeString, "%02d:%02d", hours, minutes);  // Show time with colon
  } else {
    sprintf(timeString, "%02d %02d", hours, minutes);  // Show time with space instead of colon
  }
  P.print(timeString);
}

void loop() {
  maintainWiFiConnection();
  // Handle OTA
  ArduinoOTA.handle();
  
  // Handle client requests
  server.handleClient();
  
  // Update time from NTP server
  timeClient.update();
  
  // Get hours, minutes and seconds
  hours = timeClient.getHours();
  minutes = timeClient.getMinutes();
  seconds = timeClient.getSeconds();
  
  // Update sensor readings periodically
  unsigned long currentMillis = millis();
  if (currentMillis - lastSensorUpdate >= sensorUpdateInterval) {
    lastSensorUpdate = currentMillis;
    updateSensorReadings();
  }
  
  // Update display brightness based on time if auto brightness is enabled
  if (autoBrightness) {
    if (hours < 8 || hours > 22) {
      currentBrightness = NIGHT_BRIGHTNESS;
    } else {
      currentBrightness = DAY_BRIGHTNESS;
    }
    P.setIntensity(currentBrightness);
  }
  
  // Check if it's time to toggle the colon visibility
  if (currentMillis - previousMillis >= blinkInterval) {
    previousMillis = currentMillis;
    colonVisible = !colonVisible;  // Toggle the colon visibility
    updateTimeDisplay();  // Update the display with the new colon state
  }
}

// Update all sensor readings
void updateSensorReadings() {
  // Read temperature and humidity from DHT20
  if (dht20.read() == DHT20_OK) {
    temperature = dht20.getTemperature();
    humidity = dht20.getHumidity();
    
    // Calculate heat index
    float tempF = temperature * 9.0 / 5.0 + 32;
    float hiF = calculateHeatIndex(tempF, humidity);
    heatIndex = (hiF - 32) * 5.0 / 9.0;
    
    Serial.print("Temperature: ");
    Serial.print(temperature);
    Serial.print("°C, Humidity: ");
    Serial.print(humidity);
    Serial.print("%, Heat Index: ");
    Serial.print(heatIndex);
    Serial.println("°C");
  } else {
    Serial.println("Failed to read from DHT20 sensor!");
  }
  
  // Read data from BMP280
  pressure = bmp.readPressure() / 133.3F; // Convert Pa to mmHg
  Serial.print("Pressure: ");
  Serial.print(pressure);
  Serial.print("mmHg");
  
  // Read TVOC from AGS02MA
  tvocPPB = ags.readPPB();
  if (ags.lastError() == AGS02MA_OK) {
    Serial.print("TVOC: ");
    Serial.print(tvocPPB);
    Serial.println(" ppb");
  } else {
    Serial.println("Failed to read from AGS02MA sensor!");
  }
}

// Calculate heat index using NOAA formula
float calculateHeatIndex(float tempF, float humidity) {
  // NOAA Heat Index formula (Rothfusz)
  float hi = -42.379 +
            2.04901523 * tempF +
            10.14333127 * humidity -
            0.22475541 * tempF * humidity -
            0.00683783 * tempF * tempF -
            0.05481717 * humidity * humidity +
            0.00122874 * tempF * tempF * humidity +
            0.00085282 * tempF * humidity * humidity -
            0.00000199 * tempF * tempF * humidity * humidity;

  // Adjustment for low humidity
  if (humidity < 13 && tempF >= 80 && tempF <= 112) {
    hi -= ((13 - humidity) / 4) * sqrt((17 - abs(tempF - 95)) / 17);
  }
  // Adjustment for high humidity
  else if (humidity > 85 && tempF >= 80 && tempF <= 87) {
    hi += ((humidity - 85) / 10) * ((87 - tempF) / 5);
  }
  
  return hi;
}

void handleRestart() {
  server.send(200, "text/plain", "Restarting device...");
  delay(1000);  // Give the server time to send the response
  ESP.restart();  // Restart the ESP8266
}

// Set up OTA functionality
void setupOTA() {
  // Set hostname for OTA
  ArduinoOTA.setHostname(hostname);
  
  // Set password for OTA
  ArduinoOTA.setPassword("otakorboaabarpassword?");
  
  // OTA callbacks
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }
    
    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
    
    // Display OTA update message
    P.displayClear();
    P.print("OTA...");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    
    // Display progress as percentage
    int percent = progress / (total / 100);
    char progressStr[5];
    sprintf(progressStr, "%d%%", percent);
    P.print(progressStr);
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nUpdate complete");
    P.print("Done!");
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
      P.print("Auth!");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
      P.print("Begin!");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
      P.print("Conn!");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
      P.print("Recv!");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
      P.print("End!");
    }
  });
  
  // Begin OTA
  ArduinoOTA.begin();
  Serial.println("OTA ready");
}

// Web server handlers
void handleRoot() {
  server.send(200, "text/html", MAIN_page);
}

void handleRelayOn() {
  digitalWrite(RELAY_PIN, LOW);
  relayState = true;
  server.send(200, "text/plain", "ON");
}

void handleRelayOff() {
  digitalWrite(RELAY_PIN, HIGH);
  relayState = false;
  server.send(200, "text/plain", "OFF");
}

void handleRelayStatus() {
  server.send(200, "text/plain", relayState ? "ON" : "OFF");
}

void handleSetBrightness() {
  if (server.hasArg("value")) {
    // Get brightness value from request
    int brightness = server.arg("value").toInt();
    
    // Constrain brightness to valid range
    brightness = constrain(brightness, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
    
    // Disable auto brightness when manually setting brightness
    autoBrightness = false;
    
    // Set new brightness
    currentBrightness = brightness;
    P.setIntensity(currentBrightness);
    
    // Send response
    server.send(200, "text/plain", String(currentBrightness));
  } else {
    server.send(400, "text/plain", "Missing brightness value");
  }
}

void handleGetBrightness() {
  // Create JSON response with brightness value and auto state
  String json = "{\"brightness\":" + String(currentBrightness) + ",\"auto\":" + (autoBrightness ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleAutoBrightness() {
  if (server.hasArg("state")) {
    String state = server.arg("state");
    
    if (state == "on") {
      autoBrightness = true;
      // Immediately apply time-based brightness
      if (hours < 8 || hours > 22) {
        currentBrightness = NIGHT_BRIGHTNESS;
      } else {
        currentBrightness = DAY_BRIGHTNESS;
      }
    } else {
      autoBrightness = false;
      // Keep current brightness setting
    }
    
    // Apply the brightness setting
    P.setIntensity(currentBrightness);
    
    // Send response
    server.send(200, "text/plain", String(currentBrightness));
  } else {
    server.send(400, "text/plain", "Missing state parameter");
  }
}

void handleSensorData() {
  // Force update sensor readings before sending
  updateSensorReadings();
  
  // Create JSON response with sensor data
  String json = "{";
  json += "\"temperature\":\"" + String(temperature, 1) + "\"";
  json += ",\"humidity\":\"" + String(humidity, 1) + "\"";
  json += ",\"heatIndex\":\"" + String(heatIndex, 1) + "\"";
  json += ",\"tvoc\":\"" + String(tvocPPB) + "\"";
  json += ",\"pressure\":\"" + String(pressure, 1) + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

void maintainWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long currentMillis = millis();
    if (currentMillis - lastReconnectTime >= 10000) { // Attempt reconnect every 10 seconds
      lastReconnectTime = currentMillis;
      WiFi.reconnect();
    }
  }
}

void handleDeviceInfo() {
  // Create JSON response with device info
  String json = "{\"hostname\":\"" + String(hostname) + "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
  server.send(200, "application/json", json);
}