#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Wire.h>
#include <BH1750.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ModbusMaster.h>

//=============== PIN DEFINITIONS ===============
// Pumps/Relays
#define RELAY1_PIN 25  // Irrigation Pump
#define RELAY2_PIN 33  // Fertilizer Pump

// I2C for BME280 and Light Sensor (shared I2C bus)
#define I2C_SDA 21
#define I2C_SCL 22

// RS485 for NPK Sensor
#define NPK_RX 16        // RX2 (ESP32-WROOM)
#define NPK_TX 17        // TX2
#define RE_DE_PIN 4      // RS485 direction control

//=============== CONSTANTS ===============
// NPK Sensor Soil Moisture Calibration
const float NPK_DRY_VALUE = 23.1;     // NPK sensor reading for dry soil (0% moisture)
const float NPK_WET_VALUE = 67.0;     // NPK sensor reading for muddy soil (100% moisture)

// Threshold values for automatic control
const float SOIL_MOISTURE_THRESHOLD_LOW = 45.0;   // Turn on irrigation if moisture below 45%
const float SOIL_MOISTURE_THRESHOLD_HIGH = 60.0;  // Turn off irrigation if moisture above 60%
const unsigned long PUMP_MAX_DURATION = 5000;     // Maximum pump runtime (5 seconds)
const unsigned long PUMP_CHECK_INTERVAL = 300000;  // Check irrigation every 5 minutes (300000ms)
const unsigned long FERTILIZER_CHECK_INTERVAL = 3600000;  // Check fertilizer every 60 minutes (3600000ms)
const unsigned long FERTILIZER_MINIMUM_INTERVAL = 43200000;  // Minimum 12 hours between fertilizing (43200000ms)

// NPK Thresholds
const int NITROGEN_THRESHOLD = 40;     // Minimum required Nitrogen (mg/kg)
const int PHOSPHORUS_THRESHOLD = 30;   // Minimum required Phosphorus (mg/kg)
const int POTASSIUM_THRESHOLD = 150;   // Minimum required Potassium (mg/kg)

// WiFi credentials
const char* ssid = "";          // Enter Wifi Name
const char* password = "";      // Enter Wifi Password
const char* serverUrl = "";     // Enter http url

// BME280 I2C address (0x76 or 0x77 depending on SDO pin)
#define BME280_ADDRESS 0x76

//=============== NPK SENSOR DATA STRUCTURE ===============
struct NPKData {
    float soilMoisture;      // 0x0000 - Soil moisture (%)
    float soilTemperature;   // 0x0001 - Soil temperature (°C)
    int nitrogen;            // 0x0004 - Nitrogen content (mg/kg)
    int phosphorus;          // 0x0005 - Phosphorus content (mg/kg)
    int potassium;           // 0x0006 - Potassium content (mg/kg)
    bool dataValid;          // Flag to indicate if data reading was successful
};

//=============== GLOBAL OBJECTS ===============
// Initialize sensors and communication
Adafruit_BME280 bme; // I2C interface
BH1750 lightMeter;
HardwareSerial NPKSerial(2);     // Use Serial2 on ESP32
ModbusMaster node;

//=============== GLOBAL VARIABLES ===============
// Pump control variables
bool pumpIrrigationActive = false;
bool pumpFertilizerActive = false;
unsigned long pumpIrrigationStartTime = 0;
unsigned long pumpFertilizerStartTime = 0;
unsigned long lastPumpCheckTime = 0;        // Track when we last checked irrigation
unsigned long lastFertilizerCheckTime = 0;  // Track when we last checked fertilizer needs
unsigned long lastFertilizerTime = 0;       // Track when we last applied fertilizer

// NPK sensor data
NPKData npkData;

//=============== FUNCTION DECLARATIONS ===============
void printCommands();
void controlPump(int pumpPin, bool &pumpActive, unsigned long &startTime, bool turnOn);
void checkPumpTimeout();
void automaticPumpControl(float soilMoisture, const NPKData &npkData);
void sendDataToServer(float temperature, float humidity, float pressure, float lux, NPKData &npkData);
void preTransmission();
void postTransmission();
bool readNPKSensor(NPKData &data);
void printNPKData(const NPKData &data);
float calibrateNPKSoilMoisture(float rawValue);

//=============== RS485 CONTROL FUNCTIONS ===============
void preTransmission() {
    digitalWrite(RE_DE_PIN, HIGH);
}

void postTransmission() {
    digitalWrite(RE_DE_PIN, LOW);
}

//=============== NPK SOIL MOISTURE CALIBRATION FUNCTION ===============
float calibrateNPKSoilMoisture(float rawValue) {
    // Constrain the raw value to expected range
    rawValue = constrain(rawValue, NPK_DRY_VALUE, NPK_WET_VALUE);
    
    // Map raw value to 0-100% scale
    float calibratedPercent = ((rawValue - NPK_DRY_VALUE) / (NPK_WET_VALUE - NPK_DRY_VALUE)) * 100.0;
    
    // Ensure result is within 0-100% range
    calibratedPercent = constrain(calibratedPercent, 0.0, 100.0);
    
    return calibratedPercent;
}

//=============== NPK SENSOR READING FUNCTION ===============
bool readNPKSensor(NPKData &data) {
    // Initialize data structure
    data.dataValid = false;
    
    // Read specific registers we need: 0x0000, 0x0001, 0x0004, 0x0005, 0x0006
    // We'll read 0x0000-0x0006 (7 registers) to get all needed data
    uint8_t result = node.readHoldingRegisters(0x0000, 7);
    
    if (result == node.ku8MBSuccess) {
        // Parse the response according to the datasheet
        
        // Register 0x0000: Moisture content (expanded by 10 times)
        uint16_t moistureRaw = node.getResponseBuffer(0);
        float rawMoisture = moistureRaw / 10.0;
        data.soilMoisture = calibrateNPKSoilMoisture(rawMoisture);
        
        // Register 0x0001: Temperature (expanded by 10 times, with complement for negative)
        uint16_t tempRaw = node.getResponseBuffer(1);
        if (tempRaw > 0x8000) {
            // Negative temperature (complement form)
            data.soilTemperature = -((0xFFFF - tempRaw + 1) / 10.0);
        } else {
            data.soilTemperature = tempRaw / 10.0;
        }
        
        // Register 0x0004: Nitrogen content (mg/kg) - index 4 in response
        data.nitrogen = node.getResponseBuffer(4);
        
        // Register 0x0005: Phosphorus content (mg/kg) - index 5 in response
        data.phosphorus = node.getResponseBuffer(5);
        
        // Register 0x0006: Potassium content (mg/kg) - index 6 in response
        data.potassium = node.getResponseBuffer(6);
        
        data.dataValid = true;
        return true;
    } else {
        Serial.printf("⚠ Modbus Error: %d (Failed to read NPK sensor)\n", result);
        return false;
    }
}

//=============== NPK DATA PRINTING FUNCTION ===============
void printNPKData(const NPKData &data) {
    if (data.dataValid) {
        Serial.println("=== NPK Sensor Data ===");
        Serial.printf("Soil Moisture (Calibrated): %.1f%%\n", data.soilMoisture);
        Serial.printf("Soil Temperature: %.1f°C\n", data.soilTemperature);
        Serial.printf("Nitrogen (N): %d mg/kg\n", data.nitrogen);
        Serial.printf("Phosphorus (P): %d mg/kg\n", data.phosphorus);
        Serial.printf("Potassium (K): %d mg/kg\n", data.potassium);
        Serial.println("======================");
    } else {
        Serial.println("⚠ NPK sensor data not available");
    }
}

//=============== SETUP ===============
void setup() {
    Serial.begin(115200);
    Serial.println("\nInitializing Smart Chilli Farm System...");
    
    // Initialize I2C
    Wire.begin(I2C_SDA, I2C_SCL);
    Serial.println("1. I2C bus initialized");

    // Initialize BME280 sensor
    if (!bme.begin(BME280_ADDRESS)) {
        Serial.println("⚠ Could not find BME280 sensor, check wiring!");
        while (1) delay(10);
    }
    
    // Configure BME280 settings for optimal performance
    bme.setSampling(Adafruit_BME280::MODE_NORMAL,     // Operating Mode
                    Adafruit_BME280::SAMPLING_X2,     // Temp. oversampling
                    Adafruit_BME280::SAMPLING_X16,    // Pressure oversampling
                    Adafruit_BME280::SAMPLING_X1,     // Humidity oversampling
                    Adafruit_BME280::FILTER_X16,      // Filtering
                    Adafruit_BME280::STANDBY_MS_500); // Standby time
    
    Serial.println("2. BME280 sensor initialized");
    delay(1000);  // Give BME280 time to stabilize

    // Relay setup
    pinMode(RELAY1_PIN, OUTPUT);
    pinMode(RELAY2_PIN, OUTPUT);
    digitalWrite(RELAY1_PIN, HIGH);  // Relays are typically active LOW
    digitalWrite(RELAY2_PIN, HIGH);  // Start with pumps OFF
    Serial.println("3. Relay pins initialized");

    // BH1750 light sensor setup (shares I2C bus with BME280)
    if (lightMeter.begin()) {
        Serial.println("4. BH1750 light sensor ready");
    } else {
        Serial.println("⚠ Error initializing BH1750!");
    }

    // RS485 DE/RE pin setup for NPK
    pinMode(RE_DE_PIN, OUTPUT);
    digitalWrite(RE_DE_PIN, LOW);
    
    // Init Modbus on Serial2 for NPK
    NPKSerial.begin(4800, SERIAL_8N1, NPK_RX, NPK_TX);
    node.begin(1, NPKSerial);  // Slave ID = 1 (default address from datasheet)
    node.preTransmission(preTransmission);
    node.postTransmission(postTransmission);
    Serial.println("5. NPK sensor initialized");

    // Connect to WiFi
    WiFi.begin(ssid, password);
    Serial.print("6. Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n   WiFi connected successfully!");
    Serial.print("   IP Address: ");
    Serial.println(WiFi.localIP());

    // Print NPK soil moisture calibration info
    Serial.println("\n=== NPK Soil Moisture Calibration ===");
    Serial.printf("Dry soil reading: %.1f%% (mapped to 0%%)\n", NPK_DRY_VALUE);
    Serial.printf("Wet soil reading: %.1f%% (mapped to 100%%)\n", NPK_WET_VALUE);
    Serial.println("====================================");

    Serial.println("\nSystem Ready!");
    printCommands();
}

//=============== HELPER FUNCTIONS ===============
void printCommands() {
    Serial.println("\n=== Available Commands ===");
    Serial.println("1: Turn Irrigation Pump ON");
    Serial.println("2: Turn Irrigation Pump OFF");
    Serial.println("3: Turn Fertilizer Pump ON");
    Serial.println("4: Turn Fertilizer Pump OFF");
    Serial.println("5: Toggle Automatic Mode");
    Serial.println("h: Show this help menu");
    Serial.println("=====================");
}

void controlPump(int pumpPin, bool &pumpActive, unsigned long &startTime, bool turnOn) {
    if (turnOn && !pumpActive) {
            digitalWrite(pumpPin, LOW);  // Active LOW
            pumpActive = true;
        startTime = millis();
            Serial.println(pumpPin == RELAY1_PIN ? "Irrigation Pump ON" : "Fertilizer Pump ON");
    } else if (!turnOn && pumpActive) {
            digitalWrite(pumpPin, HIGH);  // Active LOW
            pumpActive = false;
        if (pumpPin == RELAY2_PIN) {  // If it's the fertilizer pump
            lastFertilizerTime = millis();  // Record the fertilizer application time
        }
        Serial.println(pumpPin == RELAY1_PIN ? "Irrigation Pump OFF" : "Fertilizer Pump OFF");
    }
}

void checkPumpTimeout() {
    unsigned long currentTime = millis();
    
    // Check irrigation pump timeout
    if (pumpIrrigationActive && (currentTime - pumpIrrigationStartTime >= PUMP_MAX_DURATION)) {
        controlPump(RELAY1_PIN, pumpIrrigationActive, pumpIrrigationStartTime, false);
        Serial.println("⚠ Irrigation pump timeout reached!");
    }
    
    // Check fertilizer pump timeout
    if (pumpFertilizerActive && (currentTime - pumpFertilizerStartTime >= PUMP_MAX_DURATION)) {
        controlPump(RELAY2_PIN, pumpFertilizerActive, pumpFertilizerStartTime, false);
        Serial.println("⚠ Fertilizer pump timeout reached!");
    }
}

void automaticPumpControl(float soilMoisture, const NPKData &npkData) {
    unsigned long currentTime = millis();
    
    // Check irrigation conditions every 5 minutes
    if (currentTime - lastPumpCheckTime >= PUMP_CHECK_INTERVAL) {
        lastPumpCheckTime = currentTime;
        
        // Control irrigation pump based on soil moisture reading
    if (soilMoisture < SOIL_MOISTURE_THRESHOLD_LOW) {
            controlPump(RELAY1_PIN, pumpIrrigationActive, pumpIrrigationStartTime, true);
    } else if (soilMoisture > SOIL_MOISTURE_THRESHOLD_HIGH) {
            controlPump(RELAY1_PIN, pumpIrrigationActive, pumpIrrigationStartTime, false);
        }
    }
    
    // Check fertilizer conditions every 60 minutes
    if (currentTime - lastFertilizerCheckTime >= FERTILIZER_CHECK_INTERVAL) {
        lastFertilizerCheckTime = currentTime;
        
        // Check if NPK data is valid before proceeding
        if (npkData.dataValid) {
            bool needsFertilizer = (npkData.nitrogen < NITROGEN_THRESHOLD ||
                                  npkData.phosphorus < PHOSPHORUS_THRESHOLD ||
                                  npkData.potassium < POTASSIUM_THRESHOLD);
            
            bool moistureOK = soilMoisture > SOIL_MOISTURE_THRESHOLD_LOW;
            bool timeOK = (currentTime - lastFertilizerTime) >= FERTILIZER_MINIMUM_INTERVAL;
            
            if (needsFertilizer && moistureOK && timeOK) {
                controlPump(RELAY2_PIN, pumpFertilizerActive, pumpFertilizerStartTime, true);
                Serial.println("Applying fertilizer based on NPK levels:");
                Serial.printf("N: %d (min: %d), P: %d (min: %d), K: %d (min: %d)\n",
                            npkData.nitrogen, NITROGEN_THRESHOLD,
                            npkData.phosphorus, PHOSPHORUS_THRESHOLD,
                            npkData.potassium, POTASSIUM_THRESHOLD);
            }
        }
    }
}

void sendDataToServer(float temperature, float humidity, float pressure, float lux, NPKData &npkData) {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        
        // Prepare JSON data
        String jsonData = "{";
        jsonData += "\"temperature\":" + String(temperature, 1) + ",";
        jsonData += "\"humidity\":" + String(humidity, 1) + ",";
        jsonData += "\"pressure\":" + String(pressure, 1) + ",";
        jsonData += "\"light\":" + String(lux, 1) + ",";
        jsonData += "\"pump_irrigation\":" + String(pumpIrrigationActive ? "true" : "false") + ",";
        jsonData += "\"pump_fertilizer\":" + String(pumpFertilizerActive ? "true" : "false");
        
        // Add NPK sensor data
        if (npkData.dataValid) {
            jsonData += ",\"npk_soil_moisture\":" + String(npkData.soilMoisture, 1);
            jsonData += ",\"npk_soil_temperature\":" + String(npkData.soilTemperature, 1);
            jsonData += ",\"npk_nitrogen\":" + String(npkData.nitrogen);
            jsonData += ",\"npk_phosphorus\":" + String(npkData.phosphorus);
            jsonData += ",\"npk_potassium\":" + String(npkData.potassium);
        } else {
            jsonData += ",\"npk_soil_moisture\":null";
            jsonData += ",\"npk_soil_temperature\":null";
            jsonData += ",\"npk_nitrogen\":null";
            jsonData += ",\"npk_phosphorus\":null";
            jsonData += ",\"npk_potassium\":null";
        }
        jsonData += "}";
        
        http.begin(serverUrl);
        http.addHeader("Content-Type", "application/json");
        
        int httpResponseCode = http.POST(jsonData);
        
        if (httpResponseCode > 0) {
            String response = http.getString();
            Serial.println("HTTP Response code: " + String(httpResponseCode));
            Serial.println("Data sent successfully!");
        } else {
            Serial.println("Error sending HTTP request");
            Serial.println("HTTP Response code: " + String(httpResponseCode));
        }
        
        http.end();
    } else {
        Serial.println("⚠ WiFi not connected");
    }
}

//=============== MAIN LOOP ===============
void loop() {
    static unsigned long lastReadTime = 0;
    unsigned long currentTime = millis();
    
    // Check if it's time to read sensors and send data (every 30 seconds)
    if (currentTime - lastReadTime >= 30000) {
        lastReadTime = currentTime;

    // Read BME280 sensor
    float temperature = bme.readTemperature();
    float humidity = bme.readHumidity();
        float pressure = bme.readPressure() / 100.0F;

    // Read light sensor
    float lux = lightMeter.readLightLevel();

        // Read NPK sensor
    readNPKSensor(npkData);

        // Run automatic control with both soil moisture and NPK data
        if (npkData.dataValid) {
            automaticPumpControl(npkData.soilMoisture, npkData);
        }
        
        // Print readings and send data to server
    Serial.println("\n===== Environmental Sensor Readings =====");
    Serial.print("Air Temperature (°C): "); Serial.println(temperature, 1);
    Serial.print("Air Humidity (%): "); Serial.println(humidity, 1);
    Serial.print("Air Pressure (hPa): "); Serial.println(pressure, 1);
    Serial.print("Light Intensity (lux): "); Serial.println(lux, 1);
    Serial.print("Irrigation Pump: "); Serial.println(pumpIrrigationActive ? "ON" : "OFF");
    Serial.print("Fertilizer Pump: "); Serial.println(pumpFertilizerActive ? "ON" : "OFF");
    Serial.println("==========================================");

    // Print NPK sensor readings
    printNPKData(npkData);

        // Send all data to server
        sendDataToServer(temperature, humidity, pressure, lux, npkData);
    }

    // Check pump timeouts
    checkPumpTimeout();

    // Handle serial commands
    if (Serial.available() > 0) {
        char cmd = Serial.read();
        switch (cmd) {
            case '1':
                controlPump(RELAY1_PIN, pumpIrrigationActive, pumpIrrigationStartTime, true);
                break;
            case '2':
                controlPump(RELAY1_PIN, pumpIrrigationActive, pumpIrrigationStartTime, false);
                break;
            case '3':
                controlPump(RELAY2_PIN, pumpFertilizerActive, pumpFertilizerStartTime, true);
                break;
            case '4':
                controlPump(RELAY2_PIN, pumpFertilizerActive, pumpFertilizerStartTime, false);
                break;
            case 'h':
                printCommands();
                break;
        }
    }
    
    // Short delay to prevent overwhelming the CPU
    delay(100);
}