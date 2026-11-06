#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <base64.h>
#include <WebServer.h>

// Pin definitions for AI Thinker ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// WiFi credentials
const char* ssid = "";          // Enter Wifi Name
const char* password = "";      // Enter Wifi Password

// Flask server address
const char* serverUrl = "";     // Enter http url

// Create web server
WebServer server(80);

void setup() {
    Serial.begin(115200);
    Serial.println("\nESP32-CAM Image Capture System");

    // Configure camera
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    // Init with high specs to pre-allocate larger buffers
    if (psramFound()) {
        config.frame_size = FRAMESIZE_UXGA;
        config.jpeg_quality = 10;  // 0-63, lower means higher quality
        config.fb_count = 2;
    } else {
        config.frame_size = FRAMESIZE_SVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
    }

    // Initialize camera
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x", err);
        return;
    }

    // Set camera parameters
    sensor_t * s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_VGA); // Set lower resolution for faster upload
    s->set_quality(s, 10); // Lower quality for faster upload
    s->set_vflip(s, 1);   // Flip vertically
    s->set_hmirror(s, 1); // Mirror horizontally (optional, try 0 or 1)

    // Connect to WiFi
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // Set up web server endpoints
    server.on("/capture", HTTP_GET, handleCapture);
    server.begin();
    Serial.println("Web server started");
}

void handleCapture() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (captureAndSendImage()) {
        server.send(200, "text/plain", "Image captured and sent successfully");
    } else {
        server.send(500, "text/plain", "Failed to capture or send image");
    }
}

bool captureAndSendImage() {
    // Capture image
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        return false;
    }

    // Create JSON document
    DynamicJsonDocument doc(20000); // Adjust size based on your needs
    
    // Convert image to base64
    String base64Image = base64::encode(fb->buf, fb->len);
    
    // Add to JSON
    doc["image"] = base64Image;

    // Release camera buffer
    esp_camera_fb_return(fb);

    // Send to server
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(serverUrl);
        http.addHeader("Content-Type", "application/json");

        // Serialize JSON to string
        String jsonString;
        serializeJson(doc, jsonString);

        // Send POST request
        int httpResponseCode = http.POST(jsonString);

        if (httpResponseCode > 0) {
            String response = http.getString();
            Serial.println("HTTP Response code: " + String(httpResponseCode));
            Serial.println("Response: " + response);
            http.end();
            return true;
        } else {
            Serial.println("Error on sending POST: " + String(httpResponseCode));
            http.end();
            return false;
        }
    }
    return false;
}

void loop() {
    server.handleClient();
    delay(10);
} 