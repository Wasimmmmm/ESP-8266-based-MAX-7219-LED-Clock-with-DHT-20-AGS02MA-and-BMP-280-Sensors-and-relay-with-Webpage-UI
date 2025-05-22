#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <Wire.h>
#include <DHT20.h>
#include <AGS02MA.h>
#include <Adafruit_BMP280.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <digitalWriteFast.h>

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
unsigned long epoch = 0;
int hours = 0, minutes = 0, seconds = 0;
unsigned long lastTimeUpdate = 0;
const long timeUpdateInterval = 60000; // Update time from main device every minute
unsigned long lastInternalUpdate = 0;
const long internalUpdateInterval = 1000; // Internal time update every second
char timeString[6]; // To store the formatted time string (HH:MM)
bool relayState = false; // Track relay state
int currentBrightness = DAY_BRIGHTNESS; // Track current brightness
bool autoBrightness = true; // Track if auto brightness is enabled
unsigned long lastReconnectTime = 0;

// Variables for blinking colon
bool colonVisible = true;
unsigned long previousMillis = 0;
const long blinkInterval = 1000; // Blink every 1000ms (1 second)

void setup() {
  Serial.begin(115200);
  
  // Initialize the relay pin as output and turn it off
  pinMode(RELAY_PIN, OUTPUT);
  digitalWriteFast(RELAY_PIN, HIGH);
  
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
  
  
  WiFi.begin("JARVIS-child", "ekhaneopassword");
  WiFi.config(IPAddress(192,168,4,50), WiFi.gatewayIP(), WiFi.subnetMask());
  
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
  
  // Set up web server routes
  server.on("/relay/on", handleRelayOn);
  server.on("/relay/off", handleRelayOff);
  server.on("/relay/status", handleRelayStatus);
  server.on("/brightness/set", handleSetBrightness);
  server.on("/brightness/get", handleGetBrightness);
  server.on("/brightness/auto", handleAutoBrightness);
  server.on("/device/info", handleDeviceInfo);
  server.on("/device/restart", handleRestart);
  server.on("/sensor/data", handleSensorData);
  server.on("/relay/state", HTTP_GET, []() {
  server.send(200, "text/plain", digitalReadFast(RELAY_PIN) == LOW ? "ON" : "OFF");
  });

  server.on("/relay/toggle", HTTP_POST, []() {
    digitalWriteFast(RELAY_PIN, !digitalReadFast(RELAY_PIN));
    server.send(200, "text/plain", "OK");
  });

  server.on("/brightness", HTTP_POST, []() {
    if(server.hasArg("value")) {
      currentBrightness = server.arg("value").toInt();
      P.setIntensity(currentBrightness);
    }
    server.send(200, "text/plain", String(currentBrightness));
  });

  server.on("/sensor/api", HTTP_GET, []() {
    String json = "{";
    json += "\"temperature\":" + String(temperature,1) + ",";
    json += "\"humidity\":" + String(humidity,1) + ",";
    json += "\"pressure\":" + String(pressure,1) + ",";
    json += "\"tvoc\":" + String(tvocPPB);
    json += "}";
    server.send(200, "application/json", json);
  });
  // Start the server
  server.begin();
  Serial.println("HTTP server started");

  updateTimeFromMainDevice();
  
  // Initialize time string with colon visible
  updateTimeDisplay();
  
  // Initial sensor readings
  updateSensorReadings();
  
}

void updateTimeFromMainDevice() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    
    // Connect to the main device time API
    // Note: Use the actual IP of your main device
    http.begin(client, "http://192.168.4.1/api/time");
    
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      
      // Parse JSON response
      DynamicJsonDocument doc(256);
      DeserializationError error = deserializeJson(doc, payload);
      
      if (!error) {
        epoch = doc["epoch"];
        hours = doc["hours"];
        minutes = doc["minutes"];
        seconds = doc["seconds"];
        
        Serial.println("Time synced: " + String(hours) + ":" + String(minutes) + ":" + String(seconds));
      } else {
        Serial.println("Failed to parse time JSON");
      }
    } else {
      Serial.println("Failed to get time from main device, HTTP error: " + String(httpCode));
    }
    
    http.end();
  }
}

// New function to increment internal time between updates from main device
void updateInternalTime() {
  seconds++;
  
  if (seconds >= 60) {
    seconds = 0;
    minutes++;
    
    if (minutes >= 60) {
      minutes = 0;
      hours++;
      
      if (hours >= 24) {
        hours = 0;
      }
    }
  }
  
  // Also update epoch time
  epoch++;
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

  
  // Update sensor readings periodically
  unsigned long currentMillis = millis();
  if (currentMillis - lastSensorUpdate >= sensorUpdateInterval) {
    lastSensorUpdate = currentMillis;
    updateSensorReadings();
  }
  if (currentMillis - lastTimeUpdate >= timeUpdateInterval) {
    lastTimeUpdate = currentMillis;
    updateTimeFromMainDevice();
  }
  if (currentMillis - lastInternalUpdate >= internalUpdateInterval) {
    lastInternalUpdate = currentMillis;
    updateInternalTime();
    
    // Toggle colon visibility
    colonVisible = !colonVisible;
    updateTimeDisplay();
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
  ArduinoOTA.setPassword("SECRETOTAPASS");
  
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

void handleRelayOn() {
  digitalWriteFast(RELAY_PIN, LOW);
  relayState = true;
  server.send(200, "text/plain", "ON");
}

void handleRelayOff() {
  digitalWriteFast(RELAY_PIN, HIGH);
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
