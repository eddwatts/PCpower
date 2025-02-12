#include <Arduino.h>
#include <Keyboard.h>
#include <pico/cyw43_arch.h>
#include "pico/stdlib.h" // Include for random number generation
#include "hardware/adc.h"
#include <hardware/watchdog.h>  // Include watchdog for reset
#include <Crypto.h>
#include <AES.h>
#include <string.h>
#include "fauxmoESP.h"
#include <LittleFS.h>
#include <AsyncWebServer_RP2040W.h>  // Include the AsyncWebServer library

fauxmoESP fauxmo;

// -----------------------------------------------------------------------------
const uint8_t aesKey[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6, 0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};
const char* encryptionKey = "YourEncryptionKey123";
// WiFi credentials
const char* apSSID = "WiFiPCRemote";
const char* apPassword = "setup1234"; // AP mode password
// File to store WiFi credentials
const char* wifiCredentialsFile = "/wifi.txt";
const char* configFileName = "/config.txt";
// Variables to store WiFi credentials
AsyncWebServer server(80);
String ssid = "";
String password = "";
bool isInAPMode = true; // Flag to track if we're in AP mode or not
#define SERIAL_BAUDRATE     115200
// there are no buttons attatched to pico, The pico is pussing the power and reset buttons on a PC via opto-isolators
#define PCPower 8 // OUTPUT - via opto-isolator for momentry power button to computer
#define Reset 12 // OUTPUT - via opto-isolator for momentry reset button to computer
#define PowerLed 4 // Input - Real-world power state via opto-isolator, active low

bool PClastPowerState=false; //Power of PC
bool powerToggleRequested = false;  // Flag to toggle PC power
bool forceOffRequested = false;     // Flag to force PC off
bool resetRequested = false;        // Flag to reset PC

// Configurable prefix for device names
//const char* devicePrefix = "My PC"; // Change this to customize for different computers
String devicePrefix = "My PC";
int forceOffTimeout = 15000; // Default 15 seconds
int debounceTime = 50;       // Default 50ms for debouncing
String shortcut1Name = "Shortcut 1"; // Default names
String shortcut2Name = "Shortcut 2";
String shortcut3Name = "Shortcut 3";
// -----------------------------------------------------------------------------
// Function to load configuration from LittleFS
// Function to load configuration from LittleFS
bool loadConfig() {
  if (LittleFS.begin()) {
    File configFile = LittleFS.open(configFileName, "r");
    if (configFile) {
      while (configFile.available()) {
        String line = configFile.readStringUntil('\n');
        line.trim();

        int equalsIndex = line.indexOf('=');
        if (equalsIndex > 0) {
          String key = line.substring(0, equalsIndex);
          String value = line.substring(equalsIndex + 1);

          if (key == "devicePrefix") {
            devicePrefix = value;
          } else if (key == "forceOffTimeout") {
            forceOffTimeout = value.toInt();
          } else if (key == "debounceTime") {
            debounceTime = value.toInt();
          } else if (key == "shortcut1Name") {
            shortcut1Name = value;
          } else if (key == "shortcut2Name") {
            shortcut2Name = value;
          } else if (key == "shortcut3Name") {
            shortcut3Name = value;
          }
        }
      }
      configFile.close();
      return true;
    } else {
      Serial.println("Creating default config file...");
      if (createDefaultConfigFile()) {
        return loadConfig();
      } else {
        return false;
      }
    }
  } else {
    Serial.println("Failed to mount LittleFS for config");
    return false;
  }
}

// Function to create a default config file
bool createDefaultConfigFile() {
  File configFile = LittleFS.open(configFileName, "w");
  if (configFile) {
    configFile.println("devicePrefix=My PC"); // Example
    configFile.println("forceOffTimeout=15000"); // Example
    configFile.println("debounceTime=50"); // Example
    configFile.println("shortcut1Name=Shortcut 1"); // Default names
    configFile.println("shortcut2Name=Shortcut 2");
    configFile.println("shortcut3Name=Shortcut 3");    configFile.close();
    return true;
  } else {
    Serial.println("Failed to create default config file");
    return false;
  }
}

bool saveConfig(const String& newDevicePrefix, int newForceOffTimeout, int newDebounceTime, const String& newShortcut1Name, const String& newShortcut2Name, const String& newShortcut3Name) {
  File configFile = LittleFS.open(configFileName, "w");
  if (configFile) {
    configFile.println("devicePrefix=" + newDevicePrefix);
    configFile.println("forceOffTimeout=" + String(newForceOffTimeout));
    configFile.println("debounceTime=" + String(newDebounceTime));
    configFile.println("shortcut1Name=" + newShortcut1Name); // Save shortcut names
    configFile.println("shortcut2Name=" + newShortcut2Name);
    configFile.println("shortcut3Name=" + newShortcut3Name);    configFile.close();
    return true;
  } else {
    Serial.println("Failed to open config file for writing");
    return false;
  }
}


void resetPico() {
  Serial.println("Resetting Pico...");
  watchdog_reboot(0, 0, 0);  // Trigger reset with default arguments (0 will reset the system)
}

void setupWatchdog() {
  // Initialize the watchdog with a timeout of 4 seconds (4000 ms)
  watchdog_enable(5000, 1);  // Enable the watchdog timer with 5 seconds timeout
}

float readInternalTemperature() {
    adc_select_input(4); // Select the temperature sensor input (ADC channel 4)
    uint16_t raw_value = adc_read(); // Read the raw ADC value
    float voltage = raw_value * 3.3f / (1 << 12); // Convert to voltage (3.3V reference, 12-bit ADC)
    float temperature = 27.0f - (voltage - 0.706f) / 0.001721f; // Convert to temperature in °C
    return temperature;
}
uint32_t generateRandomSeed() {
    uint32_t seed = 0;

    // 1. Add noise from the internal temperature sensor
    float temperature = readInternalTemperature();
    seed ^= (uint32_t)(temperature * 1000); // Multiply by 1000 to get more significant bits

    // 2. Add noise from ADC (if available)
    adc_select_input(0); // Select ADC input 0 (GPIO 26)
    uint16_t adc_value = adc_read();
    seed ^= adc_value;

    // 3. Add some pseudo-randomness from a fixed point
    seed ^= 0x15B233FF; // A somewhat arbitrary constant
    seed ^= (uint32_t)&__bss_end__; // Use the address of a known symbol

    // 4. Seed the PRNG
    randomSeed(seed);

    return seed;
}

// Function to start AP mode
void startAPMode() {
  isInAPMode = true; // No credentials, starting in AP mode
  WiFi.softAP(apSSID, apPassword);
  Serial.println("AP Mode Started");
  Serial.print("IP Address: ");
  Serial.println(WiFi.softAPIP());
}

// Function to scan available WiFi networks
String scanWiFiNetworks() {
  int n = WiFi.scanNetworks();
  String networks = "";
  for (int i = 0; i < n; i++) {
    networks += "<option value=\"" + String(WiFi.SSID(i)) + "\">" + String(WiFi.SSID(i)) + "</option>";
  }
  WiFi.scanDelete();  // Clear the scan list after we're done
  return networks;
}

String encryptData(const String &data) {
  AES128 aes128;
  uint8_t plaintext[16];
  uint8_t ciphertext[16];
  uint8_t iv[16];

  // Generate a random IV
  for (int i = 0; i < 16; i++) {
    iv[i] = (uint8_t)random(256); // Generates a number between 0 and 255
  }

  // Pad the data to 16 bytes
  memset(plaintext, 0, 16);
  memcpy(plaintext, data.c_str(), min(data.length(), 16));

  // Encrypt the data
  aes128.setKey(aesKey, 16);
  aes128.encryptBlock(ciphertext, plaintext);

  // Prepend the IV to the ciphertext (for storage)
  String encryptedData = "";
  for (int i = 0; i < 16; i++) { // Add IV to encrypted data
    char hex[3];
    sprintf(hex, "%02X", iv[i]);
    encryptedData += hex;
  }
  for (int i = 0; i < 16; i++) { // Add Ciphertext to encrypted data
    char hex[3];
    sprintf(hex, "%02X", ciphertext[i]);
    encryptedData += hex;
  }

  return encryptedData;
}

String decryptData(const String &encryptedData) {
  AES128 aes128;
  uint8_t ciphertext[16];
  uint8_t plaintext[16];
  uint8_t iv[16];

    // Extract the IV from the beginning of the string
  for (int i = 0; i < 16; i++) {
    char hex[3];
    hex[0] = encryptedData.charAt(i * 2);
    hex[1] = encryptedData.charAt(i * 2 + 1);
    hex[2] = '\0';
    iv[i] = strtol(hex, NULL, 16);
  }

  // Extract the ciphertext after the IV (Corrected Offset!)
  for (int i = 0; i < 16; i++) {
    char hex[3];
    hex[0] = encryptedData.charAt(i * 2 + 32); // Offset is now correct!
    hex[1] = encryptedData.charAt(i * 2 + 33); // Offset is now correct!
    hex[2] = '\0';
    ciphertext[i] = strtol(hex, NULL, 16);
  }

  // Decrypt the data
  aes128.setKey(aesKey, 16);
  aes128.decryptBlock(plaintext, ciphertext);

  // Convert plaintext to a string
  String decryptedData = ""; // Declare decryptedData HERE!
  for (int i = 0; i < 16; i++) {
    if (plaintext[i] == 0) break; // Stop at null terminator
    decryptedData += (char)plaintext[i];
  }

  return decryptedData;
}

// Function to load WiFi credentials from LittleFS
bool loadWiFiCredentials() {
  if (LittleFS.begin()) {
    File file = LittleFS.open(wifiCredentialsFile, "r");
    if (file) {
      String encryptedSSID = file.readStringUntil('\n');
      String encryptedPassword = file.readStringUntil('\n');
      file.close();
      // Decrypt the credentials
      ssid = decryptData(encryptedSSID);
      password = decryptData(encryptedPassword);      
      ssid.trim();
      password.trim();
      return true;
    }
  }
  return false;
}

// Function to save WiFi credentials to LittleFS
void saveWiFiCredentials(const String& newSSID, const String& newPassword) {
  // Encrypt the credentials before saving
  String encryptedSSID = encryptData(newSSID);
  String encryptedPassword = encryptData(newPassword);

  File file = LittleFS.open(wifiCredentialsFile, "w");
  if (!file) {
    Serial.println("Failed to open wifi config for writing");
  } else {
    file.println(encryptedSSID);
    file.println(encryptedPassword);
    file.close();
  }
}

// -----------------------------------------------------------------------------
// Debounce Power LED reading
// -----------------------------------------------------------------------------
bool readPowerLed(int db1) {
    static bool lastState = digitalRead(db1);
    static unsigned long lastDebounceTime = 0;
    bool currentState = digitalRead(db1);

    if (currentState != lastState) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > debounceTime) { // 50ms debounce time
        lastState = currentState;
    }

    return !lastState;  // Inverted logic due to optocoupler (HIGH = off, LOW = on)
}
// -----------------------------------------------------------------------------
// Wifi
// -----------------------------------------------------------------------------

bool wifiSetup() {
  if (ssid.length() > 0 && password.length() > 0) {
    // Set WIFI module to STA mode
    WiFi.mode(WIFI_STA);

    // Connect
    Serial.printf("[WIFI] Connecting to %s ", ssid);
    WiFi.begin(ssid.c_str(), password.c_str());

    // Wait
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected to WiFi!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      return true;
    } else {
      Serial.println("\nFailed to connect to WiFi.");
      switch (WiFi.status()) {
          case WL_NO_SSID_AVAIL:
              Serial.println("Error: Network not found.");
              break;
          case WL_CONNECT_FAILED:
              Serial.println("Error: Connection failed (incorrect password?).");
              break;
          case WL_CONNECTION_LOST:
              Serial.println("Error: Connection lost.");
              break;
          default:
              Serial.println("Error: Unknown error.");
              break;
      }
      return false;
    }
  } else {
    Serial.println("Error: No WiFi credentials found.");
    return false;
  }
}


void APWebSetup() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
      String html ="";
      String availableNetworks = scanWiFiNetworks();  // Scan networks
      html +=R"(
      <html>
<head>
<title>WiFi Setup</title>
<style>
body {
  font-family: sans-serif;
  background-color: #f4f4f4; /* Light background color */
  display: flex;
  justify-content: center; /* Center horizontally */
  align-items: center; /* Center vertically */
  min-height: 100vh; /* Ensure full viewport height */
  margin: 0; /* Remove default margins */
}

.container {
  background-color: white;
  padding: 30px;
  border-radius: 8px;
  box-shadow: 0 2px 5px rgba(0, 0, 0, 0.1); /* Subtle shadow */
  width: 350px; /* Adjust width as needed */
}

h1 {
  text-align: center;
  color: #333;
  margin-bottom: 20px;
}

label {
  display: block; /* Make labels stack on top of inputs */
  margin-bottom: 5px;
  color: #555;
}

select,
input[type="password"] {
  width: calc(100% - 12px); /* Full width, accounting for padding */
  padding: 10px;
  margin-bottom: 15px;
  border: 1px solid #ccc;
  border-radius: 4px;
  box-sizing: border-box; /* Include padding in width calculation */
  font-size: 16px;
}

input[type="submit"] {
  background-color: #4CAF50; /* Green */
  color: white;
  padding: 12px 20px;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  font-size: 16px;
  width: 100%; /* Full width button */
  transition: background-color 0.3s; /* Smooth transition */
}

input[type="submit"]:hover {
  background-color: #45a049; /* Darker green on hover */
}

select:focus,
input[type="password"]:focus {
  outline: none; /* Remove default outline on focus */
  border-color: #2196F3; /* Blue border on focus */
  box-shadow: 0 0 5px rgba(33, 150, 243, 0.3); /* Subtle blue shadow on focus */
}

</style>
</head>      
        <body>
          <h1>WiFi Setup</h1>
          <form action="/save" method="POST">
            <select name="ssid">)";
              html += availableNetworks;  // Add the SSIDs to the dropdown
              html += R"(</select><br>
            Password: <input type="password" name="password"><br>
            <input type="submit" value="Save">
          </form>
        </body>
      </html>    )";
      request->send(200, "text/html", html);
    });
    // Handle saving WiFi credentials
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
      String ssid = request->arg("ssid");
      String password = request->arg("password");
      saveWiFiCredentials(ssid, password);
      request->send(200, "text/plain", "Credentials saved. Restarting...");
      resetPico();  // Trigger the reset here using the watchdog
    });
    server.begin();
}
void WebSetup() {
    server.on("/MyPc", HTTP_GET, [](AsyncWebServerRequest *request){
      String powerStatus = readPowerLed(PowerLed) ? "ON" : "OFF"; // Get power status string
      String html = R"=====(
      <html>
      <head>
<style>
body {
  font-family: sans-serif;
}
.container {
  display: flex;
  flex-direction: column; /* Arrange buttons vertically */
  align-items: center; /* Center buttons horizontally */
  gap: 10px; /* Space between buttons */
  padding: 20px;
  border: 1px solid #ccc;
  border-radius: 5px;
}

button {
  padding: 10px 20px;
  border: none;
  border-radius: 5px;
  cursor: pointer;
  font-size: 16px;
  transition: background-color 0.3s ease; /* Smooth transition for hover effect */
}

#powerButton {
  background-color: #4CAF50; /* Green */
  color: white;
}

#powerButton:hover {
  background-color: #45a049; /* Darker green on hover */
}


#resetButton {
  background-color: #f44336; /* Red */
  color: white;
}

#resetButton:hover {
  background-color: #da190b; /* Darker red on hover */
}

#forceOffButton {
    background-color: #ff9800; /* Orange */
    color: white;
}

#forceOffButton:hover {
    background-color: #e68a00; /* Darker orange on hover */
}

.shortcut-buttons {
    display: flex; /* Arrange shortcut buttons horizontally */
    gap: 10px; /* Space between shortcut buttons */
}


.shortcut-buttons button {
  background-color: #2196F3; /* Blue */
  color: white;
}

.shortcut-buttons button:hover {
  background-color: #1976D2; /* Darker blue on hover */
}
</style>
</head>
      <body>
        <h1>)=====" + devicePrefix + R"=====( Control</h1>
        <p>Power Status: <span id="powerStatus">)=====" + powerStatus + R"=====(</span></p>
        <form action="/pressbutton" method="POST">
          <button type="submit" name="button" value="power">Power</button>
          <button type="submit" name="button" value="reset">Reset</button>
          <button type="submit" name="button" value="forceoff">Force Off</button>

          <div class="shortcut-buttons">
            <button type="submit" name="button" value="shortcut1">)=====" + shortcut1Name + R"=====(</button>
            <button type="submit" name="button" value="shortcut2">)=====" + shortcut2Name + R"=====(</button>
            <button type="submit" name="button" value="shortcut3">)=====" + shortcut3Name + R"=====(</button>
          </div>
        </form>
        <script>
          function updatePowerStatus() {
            fetch('/power_status') // New route to get power status
              .then(response => response.text())
              .then(status => {
                document.getElementById('powerStatus').textContent = status;
              });
          }

          setInterval(updatePowerStatus, 1000); // Update every 1 seconds
        </script>
      </body>
      </html>
    )=====";
      request->send(200, "text/html", html);
    });

  server.on("/power_status", HTTP_GET, [](AsyncWebServerRequest *request){
    String powerStatus = readPowerLed(PowerLed) ? "ON" : "OFF";
    request->send(200, "text/plain", powerStatus);
  });
  
    server.on("/pressbutton", HTTP_POST, [](AsyncWebServerRequest *request){
        String button = request->arg("button");
        if (button == "power") {
            powerToggleRequested = true;
        } else if (button == "reset") {
            resetRequested = true;
        } else if (button == "forceoff") {
            forceOffRequested = true;
        } else if (button == "shortcut1") {
                Keyboard.press(KEY_LEFT_CTRL);
                Keyboard.press(KEY_F13);
                delay(100);
                Keyboard.releaseAll();
        } else if (button == "shortcut2") {
                Keyboard.press(KEY_LEFT_ALT);
                Keyboard.press(KEY_F13);
                delay(100);
                Keyboard.releaseAll();
        } else if (button == "shortcut3") {
                Keyboard.press(KEY_LEFT_CTRL);
                Keyboard.press(KEY_LEFT_ALT);
                Keyboard.press(KEY_F13);
                delay(100);
                Keyboard.releaseAll();
        }
        request->send(200, "text/plain", "Command received."); // Or redirect back to /MyPc
    });

  server.on("/save_config", HTTP_POST, [](AsyncWebServerRequest *request){
    String newDevicePrefix = request->arg("devicePrefix");
    int newForceOffTimeout = request->arg("forceOffTimeout").toInt();
    int newDebounceTime = request->arg("debounceTime").toInt();
    String newShortcut1Name = request->arg("shortcut1Name"); // Get new names
    String newShortcut2Name = request->arg("shortcut2Name");
    String newShortcut3Name = request->arg("shortcut3Name");
    if (saveConfig(newDevicePrefix, newForceOffTimeout, newDebounceTime, newShortcut1Name, newShortcut2Name, newShortcut3Name)) {
      request->send(200, "text/plain", "Configuration saved. Restarting...");
      delay(1000); // Small delay before restart
      resetPico();  // Restart the ESP to load the new config
    } else {
      request->send(500, "text/plain", "Failed to save configuration.");
    }
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = R"=====(
      <html>
      <head>
        <title>Configuration</title>
      </head>
      <body>
        <h1>Configuration</h1>
        <form action="/save_config" method="POST">
          Device Prefix: <input type="text" name="devicePrefix" value=")" + devicePrefix + R"=====("><br>
          Force Off Timeout (ms): <input type="number" name="forceOffTimeout" value=")" + String(forceOffTimeout) + R"=====("><br>
          Debounce Time (ms): <input type="number" name="debounceTime" value=")" + String(debounceTime) + R"=====("><br>
          Shortcut 1 Name: <input type="text" name="shortcut1Name" value=")" + shortcut1Name + R"=====("><br>
          Shortcut 2 Name: <input type="text" name="shortcut2Name" value=")" + shortcut2Name + R"=====("><br>
          Shortcut 3 Name: <input type="text" name="shortcut3Name" value=")" + shortcut3Name + R"=====("><br>
          <input type="submit" value="Save">
        </form>
      </body>
      </html>
    )=====";
    request->send(200, "text/html", html);
  });

    server.begin();
}

void handlePowerToggle() {
  digitalWrite(PCPower, HIGH); 
  delay(500);
  digitalWrite(PCPower, LOW);
  delay(1000);
  PClastPowerState = readPowerLed(PowerLed); // Update state immediately
  fauxmo.setState((String(devicePrefix) + " Power").c_str(), PClastPowerState, 255); // value is for dimming, not used in this example
  powerToggleRequested=false;
}

void handleForceOff() {
  watchdog_disable();
  bool pcTurnedOff = false;
  const int maxRetries = 3;
  for (int retry = 0; retry < maxRetries; retry++) {
    digitalWrite(PCPower, HIGH);
    unsigned long startTime = millis();
    while (millis() - startTime < forceOffTimeout) {
      if (!readPowerLed(PowerLed)) {
        break; // PC is off, exit retry loop
      }
      delay(100);
    }
    digitalWrite(PCPower, LOW);
    delay(1000); //pause between retrys and power state checks
    if (!readPowerLed(PowerLed)) {
      break; // PC is off, exit retry loop
    } else {
      Serial.printf("Force Off timeout! Retrying (Attempt %d)...\n", retry + 1); // Added retry number
    }
  }
  if (!readPowerLed(PowerLed)) { // Check *after* all retries are exhausted
      Serial.println("Error: PC might not have shut down after 3 attempts.");
  }
  setupWatchdog();
  PClastPowerState=readPowerLed(PowerLed);
  fauxmo.setState((String(devicePrefix) + " Force Off").c_str(), false, 255);
  fauxmo.setState((String(devicePrefix) + " Power").c_str(), readPowerLed(PowerLed), 255); // value is for dimming, not used in this example
  forceOffRequested=false;
}

void handleReset() {
  digitalWrite(Reset, HIGH); 
  delay(500);
  digitalWrite(Reset, LOW);
  delay(1000);
  fauxmo.setState((String(devicePrefix) + " Reset").c_str(), false, 255);
  fauxmo.setState((String(devicePrefix) + " Power").c_str(), readPowerLed(PowerLed), 255); // value is for dimming, not used in this example
  resetRequested=false;
}


// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(SERIAL_BAUDRATE);
    Serial.println();
    Keyboard.begin();
  // Initialize ADC
  adc_init();
  adc_set_temp_sensor_enabled(true); // Enable the internal temperature sensor

  // Generate a random seed using the temperature sensor and other sources
  generateRandomSeed();

    // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount LittleFS");
    return;
  }
    // Configure pins
    pinMode(PCPower, OUTPUT);
    pinMode(Reset, OUTPUT);
    pinMode(PowerLed, INPUT_PULLUP);
    digitalWrite(PCPower, LOW);
    digitalWrite(Reset, LOW);
    PClastPowerState = readPowerLed(PowerLed);

  if (loadConfig()) {
    Serial.println("Config loaded successfully:");
    Serial.print("Device Prefix: "); Serial.println(devicePrefix);
    Serial.print("Force Off Timeout: "); Serial.println(forceOffTimeout);
    Serial.print("Debounce Time: "); Serial.println(debounceTime);
    Serial.print("Shortcut 1 Name: "); Serial.println(shortcut1Name);
    Serial.print("Shortcut 2 Name: "); Serial.println(shortcut2Name);
    Serial.print("Shortcut 3 Name: "); Serial.println(shortcut3Name);
  } else {
    Serial.println("Failed to load config. Using defaults.");
  }


    // Connect to WiFi
  // Load saved WiFi credentials
  if (loadWiFiCredentials()) {
    Serial.println("Loaded WiFi credentials:");
    isInAPMode = false;
  }
   if (wifiSetup()) {
    isInAPMode = false;
   }

  if (isInAPMode == false) {
    // Add virtual devices for Alexa
    fauxmo.addDevice((devicePrefix + " Power").c_str());
    fauxmo.addDevice((devicePrefix + " Force Off").c_str());
    fauxmo.addDevice((devicePrefix + " Reset").c_str());
    fauxmo.addDevice((devicePrefix + " " + shortcut1Name).c_str());
    fauxmo.addDevice((devicePrefix + " " + shortcut2Name).c_str());
    fauxmo.addDevice((devicePrefix + " " + shortcut3Name).c_str());

    WebSetup();
    // Set up fauxmoESP
    fauxmo.createServer(false);
    fauxmo.setPort(80);
    fauxmo.enable(true); 



    // Callback for Alexa commands
    fauxmo.onSetState([](unsigned char device_id, const char * device_name, bool state, unsigned char value) {
        Serial.printf("[MAIN] Device #%d (%s) state: %s value: %d\n", device_id, device_name, state ? "ON" : "OFF", value);

        if (strcmp(device_name, (String(devicePrefix) + " Power").c_str()) == 0) {
            // Toggle power if the requested state doesn't match the current state
            if (state != readPowerLed(PowerLed)) {
                powerToggleRequested = true;
            }
        } else if (strcmp(device_name, (String(devicePrefix) + " Force Off").c_str()) == 0) {
            // Force PC off if it's currently on
            if (readPowerLed(PowerLed) && state) {
                fauxmo.setState((String(devicePrefix) + " Force Off").c_str(), true, 255);
                forceOffRequested = true;
            }
        } else if (strcmp(device_name, (String(devicePrefix) + " Reset").c_str()) == 0) {
            // Reset PC if it's currently on
            if (readPowerLed(PowerLed) && state) {
                fauxmo.setState((String(devicePrefix) + " Reset").c_str(), true, 255);
                resetRequested = true;
            }
        } else if (strcmp(device_name, (String(devicePrefix) + " " + String(shortcut1Name)).c_str()) == 0) {
            // Send keyboard shortcut 1
            if (readPowerLed(PowerLed) && state) {
                fauxmo.setState((String(devicePrefix) + " " + String(shortcut1Name)).c_str(), true, 255);
                Keyboard.press(KEY_LEFT_CTRL);
                Keyboard.press(KEY_F13);
                delay(100);
                Keyboard.releaseAll();
                delay(1000);
                fauxmo.setState((String(devicePrefix) + " " + String(shortcut1Name)).c_str(), false, 255);
            }
        } else if (strcmp(device_name, (String(devicePrefix) + " " + String(shortcut2Name)).c_str()) == 0) {
            // Send keyboard shortcut 2
            if (readPowerLed(PowerLed) && state) {
                fauxmo.setState((String(devicePrefix) + " " + String(shortcut2Name)).c_str(), true, 255);
                Keyboard.press(KEY_LEFT_ALT);
                Keyboard.press(KEY_F13);
                delay(100);
                Keyboard.releaseAll();
                delay(1000);
                fauxmo.setState((String(devicePrefix) + " " + String(shortcut2Name)).c_str(), false, 255);
            }
        } else if (strcmp(device_name, (String(devicePrefix) + " " + String(shortcut3Name)).c_str()) == 0) {
            // Send keyboard shortcut 3
            if (readPowerLed(PowerLed) && state) {
                fauxmo.setState((String(devicePrefix) + " " + String(shortcut3Name)).c_str(), true, 255);
                Keyboard.press(KEY_LEFT_CTRL);
                Keyboard.press(KEY_LEFT_ALT);
                Keyboard.press(KEY_F13);
                delay(100);
                Keyboard.releaseAll();
                delay(1000);
                fauxmo.setState((String(devicePrefix) + " " + String(shortcut3Name)).c_str(), false, 255);
            }
        }
    });

  } else {
    Serial.println("No WiFi credentials found. Starting AP mode...");
    startAPMode();
    APWebSetup();
  }
setupWatchdog();
}

// -----------------------------------------------------------------------------
// Main Loop
// -----------------------------------------------------------------------------

void loop() {

  // Reset the watchdog timer to avoid triggering a reset
  watchdog_update();
  if (isInAPMode == false) {
    // fauxmoESP uses an async TCP server but a sync UDP server
    // Therefore, we have to manually poll for UDP packets
    fauxmo.handle();

    // This is a sample code to output free heap every 5 seconds
    // This is a cheap way to detect memory leaks
    static unsigned long last = millis();
    if (millis() - last > 5000) {
        last = millis();
          Serial.printf("[MAIN] Free heap: %d bytes\n", rp2040.getFreeHeap());
          // only update Power Status if it has changed
          bool PCcurrentState = readPowerLed(PowerLed);
          if (PCcurrentState != PClastPowerState) {
            PClastPowerState = PCcurrentState;
            fauxmo.setState((String(devicePrefix) + " Power").c_str(), PCcurrentState, 255); // value is for dimming, not used in this example
          }
    }
         if (powerToggleRequested) {
            handlePowerToggle();
        }
        if (forceOffRequested) {
            handleForceOff();
        }
        if (resetRequested) {
            handleReset();
        }
    // If your device state is changed by any other means (MQTT, physical button,...)
    // you can instruct the library to report the new state to Alexa on next request:
    // fauxmo.setState(ID_YELLOW, true, 255);
  }
}