#pragma once

// ==========================================
// Wi-Fi Configuration
// ==========================================
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// ==========================================
// Network & OTA Configuration
// ==========================================
#define Z2M_TCP_PORT 8888
#define OTA_PASSWORD "admin"

// ==========================================
// Internal Virtual Loopback Pins (GPIO Matrix)
// ==========================================
// These pins are used internally to connect the TCP bridge to the Zigbee NCP.
// They do NOT need to be physically connected or exposed.
#define VIRTUAL_PIN_A 4
#define VIRTUAL_PIN_B 5

// ==========================================
// Hardware Pins
// ==========================================
// OLED (SSD1306 0.96 inch, I2C Address usually 0x3C)
// The module is powered by 3.3V as per configuration.
#define I2C_SDA_PIN 22
#define I2C_SCL_PIN 23

// Onboard Status LED
#define STATUS_LED_PIN 15

// ==========================================
// Zigbee Configuration
// ==========================================
#define ZIGBEE_MANUFACTURER "Custom_ESP32C6"
#define ZIGBEE_MODEL "OLED_Router_01"

// ==========================================
// OLED Configuration
// ==========================================
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
// Shift boundary for burn-in protection
#define MAX_SHIFT_X 2
#define MAX_SHIFT_Y 2
// Shift interval in milliseconds (1 minute = 60000 ms)
#define SHIFT_INTERVAL_MS 60000
