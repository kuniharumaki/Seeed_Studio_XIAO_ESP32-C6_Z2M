#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <esp_coexist.h>
#include "app.h"
#include "protocol.h"
#include "config.h"
#include "zb_ncp.h"
#include "diagnostics.h"

// ==========================================
// Global Variables & Objects
// ==========================================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, I2C_SCL_PIN, I2C_SDA_PIN);

WiFiServer server(Z2M_TCP_PORT);
WiFiClient z2mClient;

WiFiServer diagServer(80);
static volatile bool diagServerStarted = false;

// State variables
bool zigbeeConnected = false;
String displayMessage = "Waiting for setup...";
String deviceIP = "0.0.0.0";
unsigned long bridgeTxCount = 0;
unsigned long bridgeRxCount = 0;
bool isOtaUpdating = false;
volatile bool z2mClientConnected = false;

// Pixel shift variables for burn-in protection
int pixelShiftX = 0;
int pixelShiftY = 0;
unsigned long lastShiftTime = 0;

// LED variables
bool ledState = false;
unsigned long lastBlinkTime = 0;
unsigned long lastPulseTime = 0;

// ==========================================
// Helper Functions
// ==========================================

void recoverI2CBus() {
    Wire.end();
    pinMode(I2C_SDA_PIN, INPUT_PULLUP);
    pinMode(I2C_SCL_PIN, OUTPUT);
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(5);

    // Pulse SCL up to 9 times to free any stuck slave ACK
    for (int i = 0; i < 9; i++) {
        digitalWrite(I2C_SCL_PIN, LOW);
        delayMicroseconds(5);
        digitalWrite(I2C_SCL_PIN, HIGH);
        delayMicroseconds(5);
        if (digitalRead(I2C_SDA_PIN) == HIGH) {
            break;
        }
    }

    // Generate I2C STOP condition
    pinMode(I2C_SDA_PIN, OUTPUT);
    digitalWrite(I2C_SDA_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(5);
    digitalWrite(I2C_SDA_PIN, HIGH);
    delayMicroseconds(5);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(50);
}

void drawSignalBars(int x, int y, uint8_t lqi) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    // Antenna symbol 'Y'
    u8g2.drawVLine(x + 2, y + 2, 7);
    u8g2.drawLine(x, y, x + 2, y + 2);
    u8g2.drawLine(x + 4, y, x + 2, y + 2);

    // 4 Signal Bars (heights: 2, 4, 6, 8 px)
    int barX = x + 7;
    int numBars = 0;
    if (lqi >= 190) numBars = 4;
    else if (lqi >= 120) numBars = 3;
    else if (lqi >= 50) numBars = 2;
    else if (lqi >= 1) numBars = 1;

    for (int i = 0; i < 4; i++) {
        int h = 2 + i * 2; // 2, 4, 6, 8
        int bx = barX + i * 3;
        int by = y + 8 - h;
        if (by < 0) by = 0;
        if (i < numBars) {
            u8g2.drawBox(bx, by, 2, h);
        } else {
            u8g2.drawPixel(bx, y + 8); // baseline dot for inactive bar
        }
    }
}

void updateDisplay() {
    if (isOtaUpdating) return;
    u8g2.clearBuffer();
    
    int bx = (pixelShiftX < 0) ? 0 : pixelShiftX;
    int by = (pixelShiftY < 0) ? 0 : pixelShiftY;

    // Line 1: Wi-Fi status, Heap, and LQI Antenna Pictograph
    u8g2.setFont(u8g2_font_5x7_tf);
    if (WiFi.status() == WL_CONNECTED) {
        u8g2.drawStr(bx, by + 9, "WIFI:OK");
    } else {
        u8g2.drawStr(bx, by + 9, "WIFI:WAIT");
    }
    u8g2.setCursor(bx + 44, by + 9);
    u8g2.printf("H:%luK", ESP.getFreeHeap() / 1024);

    uint8_t lqi = zb_ncp::get_last_lqi();
    drawSignalBars(bx + 84, by + 1, lqi);

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.setCursor(bx + 106, by + 9);
    if (lqi > 0) {
        u8g2.printf("%3u", lqi);
    } else {
        u8g2.print("---");
    }

    u8g2.drawHLine(bx, by + 13, 128);

    // Line 2: Z2M Connection status or Reset warning
    const auto& h = Diagnostics::get_health();
    bool abnormal_reset = (h.current_reset_reason != ESP_RST_POWERON && h.current_reset_reason != ESP_RST_SW);
    if (abnormal_reset && ((millis() / 2000) % 2 == 1)) {
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.setCursor(bx, by + 28);
        u8g2.printf("! RST: %s !", h.current_reset_name);
    } else {
        u8g2.setFont(u8g2_font_7x14_tf);
        u8g2.setCursor(bx, by + 28);
        if (z2mClientConnected) {
            u8g2.print("Z2M: CONNECTED");
        } else {
            u8g2.print("Z2M: WAITING");
        }
    }
    
    // Line 3: Packet Counters
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(bx, by + 43);
    u8g2.printf("TX:%lu RX:%lu", bridgeTxCount, bridgeRxCount);
    
    u8g2.drawHLine(bx, by + 48, 128);

    // Line 4: Device IP
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.setCursor(bx, by + 58);
    u8g2.print("IP: ");
    u8g2.print(deviceIP);

    u8g2.sendBuffer();
}

void handleLED() {
    unsigned long currentMillis = millis();
    if (z2mClientConnected) {
        if (currentMillis - lastPulseTime < 50) {
            digitalWrite(STATUS_LED_PIN, LOW); // Pulse OFF
        } else {
            digitalWrite(STATUS_LED_PIN, HIGH); // Solid ON
        }
    } else {
        if (currentMillis - lastBlinkTime >= 1000) {
            lastBlinkTime = currentMillis;
            ledState = !ledState;
            digitalWrite(STATUS_LED_PIN, ledState ? HIGH : LOW);
        }
    }
}

void handlePixelShift() {
    unsigned long currentMillis = millis();
    if (currentMillis - lastShiftTime >= SHIFT_INTERVAL_MS) {
        lastShiftTime = currentMillis;
        pixelShiftX = random(0, MAX_SHIFT_X + 1);
        pixelShiftY = random(0, MAX_SHIFT_Y + 1);
    }
}

SemaphoreHandle_t client_mutex = nullptr;

void handleBridge() {
    if (!client_mutex) return;
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        Serial.println("[BRIDGE] ERROR: client_mutex take timeout!");
        return;
    }

    if (server.hasClient()) {
        if (z2mClient) {
            z2mClient.stop();
        }
        z2mClient = server.accept();
        Serial.println("Z2M Client Connected!");
        Diagnostics::log_event("Z2M Client Connected (IP: %s)", z2mClient.remoteIP().toString().c_str());
        z2mClient.setNoDelay(true);
        protocol::reset();
        z2mClientConnected = true;
    }

    bool isConnected = (z2mClient && z2mClient.connected());
    z2mClientConnected = isConnected;

    if (isConnected) {
        static unsigned long lastBridgeCheck = 0;
        if (millis() - lastBridgeCheck > 3000) {
            Serial.printf("[BRIDGE] client connected, avail=%d\n", z2mClient.available());
            lastBridgeCheck = millis();
        }

        while (z2mClient.available()) {
            size_t len = z2mClient.available();
            uint8_t* buf = (uint8_t*)malloc(len);
            if (!buf) break;
            
            size_t read_len = z2mClient.read(buf, len);
            if (read_len > 0) {
                Serial.printf("[RX<-Z2M] (%d bytes): ", (int)read_len);
                for(size_t i=0; i<read_len; i++) Serial.printf("%02X ", buf[i]);
                Serial.println();
                
                app::ctx_t ncp_event = {
                    .event = app::EVENT_OUTPUT,
                    .size = (uint16_t)read_len,
                    .buf_ptr = buf
                };
                if (app::send_event(ncp_event) != ESP_OK) {
                    free(buf);
                }
                bridgeTxCount += read_len;
                lastPulseTime = millis();
            } else {
                free(buf);
            }
        }
    }
    xSemaphoreGive(client_mutex);
}

static volatile bool z2mServerStarted = false;

void bridgeTask(void* pvParameters) {
    while (1) {
        if (WiFi.status() == WL_CONNECTED && z2mServerStarted) {
            handleBridge();
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

extern "C" {
esp_err_t esp_coex_wifi_i154_enable(void);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);
}

void handleDiagWeb() {
    if (!diagServerStarted) return;
    WiFiClient client = diagServer.accept();
    if (!client) return;

    client.setTimeout(100);

    unsigned long start = millis();
    String reqLine = "";
    while (client.connected() && millis() - start < 150) {
        if (client.available()) {
            char c = client.read();
            if (c == '\n') break;
            if (c != '\r') reqLine += c;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Abort cleanly if client sent nothing or disconnected
    if (reqLine.length() == 0 || !client.connected()) {
        client.stop();
        return;
    }

    // Drain remaining headers to ensure clean TCP socket close
    start = millis();
    int consecutiveNewlines = 0;
    while (client.connected() && millis() - start < 100 && client.available()) {
        char c = client.read();
        if (c == '\n') {
            consecutiveNewlines++;
            if (consecutiveNewlines >= 2) break;
        } else if (c != '\r') {
            consecutiveNewlines = 0;
        }
    }

    if (reqLine.startsWith("GET /api/status")) {
        const auto& h = Diagnostics::get_health();
        String json = "{";
        json += "\"uptime_sec\":" + String(millis() / 1000) + ",";
        json += "\"free_heap\":" + String(esp_get_free_heap_size()) + ",";
        json += "\"min_free_heap\":" + String(esp_get_minimum_free_heap_size()) + ",";
        json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"wifi_ch\":" + String(WiFi.channel()) + ",";
        json += "\"z2m_connected\":" + String(z2mClientConnected ? "true" : "false") + ",";
        json += "\"tx_count\":" + String(bridgeTxCount) + ",";
        json += "\"rx_count\":" + String(bridgeRxCount) + ",";
        json += "\"boot_count\":" + String(h.boot_count) + ",";
        json += "\"current_reset_reason\":\"" + String(h.current_reset_name) + "\",";
        json += "\"current_reset_desc\":\"" + String(h.current_reset_desc) + "\",";
        json += "\"prev_reset_reason\":\"" + String(h.prev_reset_name) + "\",";
        json += "\"prev_uptime_sec\":" + String(h.prev_uptime_sec) + ",";
        json += "\"prev_last_cmd\":\"0x" + String(h.prev_last_cmd_id, HEX) + " (" + String(h.prev_last_cmd_name) + ", TSN " + String(h.prev_last_tsn) + ")\",";
        json += "\"has_coredump\":" + String(h.has_coredump ? "true" : "false") + ",";
        json += "\"coredump_task\":\"" + String(h.coredump_task) + "\",";
        json += "\"coredump_pc\":\"0x" + String(h.coredump_pc, HEX) + "\",";
        json += "\"coredump_reason\":\"" + String(h.coredump_panic_reason) + "\"";
        json += "}";
        
        client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        client.print(json);
    } else if (reqLine.startsWith("GET /logs")) {
        client.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n");
        int count = Diagnostics::get_log_count();
        for (int i = 0; i < count; i++) {
            client.println(Diagnostics::get_log_entry(i));
        }
    } else if (reqLine.startsWith("GET /reboot")) {
        client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n<!DOCTYPE html><html><body style='background:#0f172a;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;'><h2>Device Rebooting...</h2><p>Please wait 10 seconds and <a href='/' style='color:#38bdf8;'>refresh the dashboard</a>.</p></body></html>\r\n");
        client.stop();
        delay(1000);
        esp_restart();
        return;
    } else if (reqLine.startsWith("GET / ") || reqLine.startsWith("GET /index.html") || reqLine.startsWith("GET / HTTP") || reqLine.startsWith("HEAD /")) {
        const auto& h = Diagnostics::get_health();
        uint32_t up = millis() / 1000;
        uint32_t up_h = up / 3600;
        uint32_t up_m = (up % 3600) / 60;
        uint32_t up_s = up % 60;
        
        uint32_t prev_h = h.prev_uptime_sec / 3600;
        uint32_t prev_m = (h.prev_uptime_sec % 3600) / 60;
        uint32_t prev_s = h.prev_uptime_sec % 60;
        
        client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n");
        client.print(R"rawhtml(
<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32-C6 Z2M Coordinator Diagnostics</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--text:#f8fafc;--muted:#94a3b8;--border:#334155;--accent:#38bdf8;--green:#22c55e;--red:#ef4444;--amber:#f59e0b;}
body{margin:0;padding:20px;background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;}
.container{max-width:850px;margin:0 auto;}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px;padding-bottom:12px;border-bottom:1px solid var(--border);}
h1{margin:0;font-size:1.3rem;color:var(--accent);}
.btn{background:var(--card);border:1px solid var(--border);color:var(--text);padding:6px 14px;border-radius:6px;cursor:pointer;text-decoration:none;font-size:0.85rem;}
.btn:hover{background:var(--border);}
.btn-red{border-color:var(--red);color:var(--red);}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px;margin-bottom:20px;}
.card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:16px;}
.card h2{margin-top:0;font-size:1rem;color:var(--muted);border-bottom:1px solid var(--border);padding-bottom:6px;}
.stat{display:flex;justify-content:space-between;padding:5px 0;font-size:0.9rem;}
.stat-val{font-weight:600;font-family:monospace;}
.badge{padding:2px 8px;border-radius:4px;font-size:0.75rem;font-weight:bold;}
.badge-green{background:#14532d;color:#4ade80;}
.badge-red{background:#7f1d1d;color:#f87171;}
.badge-amber{background:#78350f;color:#fcd34d;}
.alert-box{background:#451a03;border:1px solid var(--amber);border-radius:6px;padding:12px;margin-top:12px;font-size:0.85rem;color:#fed7aa;}
.logs{background:#020617;border:1px solid var(--border);border-radius:6px;padding:10px;font-family:monospace;font-size:0.75rem;color:#4ade80;max-height:240px;overflow-y:auto;white-space:pre-wrap;}
</style>
</head>
<body>
<div class="container">
<header>
  <div>
    <h1>Seeed XIAO ESP32-C6 Diagnostics</h1>
    <div style="font-size:0.8rem;color:var(--muted);margin-top:4px;">Zigbee2MQTT ZBOSS Coordinator</div>
  </div>
  <div>
    <a class="btn" href="/">更新 (Refresh)</a>
    <a class="btn btn-red" href="/reboot" onclick="return confirm('ESP32-C6 を再起動しますか？');">再起動 (Reboot)</a>
  </div>
</header>
<div class="grid">
)rawhtml");

        client.print("<div class=\"card\"><h2>システム稼働状況 (Health)</h2>");
        client.printf("<div class=\"stat\"><span>稼働時間 (Uptime)</span><span class=\"stat-val\">%luh %lum %lus</span></div>", (unsigned long)up_h, (unsigned long)up_m, (unsigned long)up_s);
        client.printf("<div class=\"stat\"><span>空きメモリ (Free Heap)</span><span class=\"stat-val\">%lu KB</span></div>", (unsigned long)(esp_get_free_heap_size() / 1024));
        client.printf("<div class=\"stat\"><span>最小空きメモリ (Min Heap)</span><span class=\"stat-val\">%lu KB</span></div>", (unsigned long)(esp_get_minimum_free_heap_size() / 1024));
        client.printf("<div class=\"stat\"><span>Wi-Fi 電波強度 (RSSI)</span><span class=\"stat-val\">%d dBm (Ch %d)</span></div>", WiFi.RSSI(), WiFi.channel());
        client.printf("<div class=\"stat\"><span>Z2M TCP (:8888)</span><span class=\"badge %s\">%s</span></div>",
            z2mClientConnected ? "badge-green" : "badge-red",
            z2mClientConnected ? "CONNECTED" : "WAITING");
        client.printf("<div class=\"stat\"><span>TX / RX カウンタ</span><span class=\"stat-val\">%lu / %lu</span></div>", bridgeTxCount, bridgeRxCount);
        client.print("</div>");

        bool is_abnormal = (h.current_reset_reason != ESP_RST_POWERON && h.current_reset_reason != ESP_RST_SW);
        client.print("<div class=\"card\"><h2>再起動・クラッシュ解析 (Reset & Crash)</h2>");
        client.printf("<div class=\"stat\"><span>通算起動回数 (Boot Count)</span><span class=\"stat-val\">#%lu</span></div>", (unsigned long)h.boot_count);
        client.printf("<div class=\"stat\"><span>今回の起動要因</span><span class=\"badge %s\">%s</span></div>",
            is_abnormal ? "badge-red" : "badge-green", h.current_reset_name);
        client.printf("<div class=\"stat\"><span>要因説明</span><span style=\"font-size:0.8rem;text-align:right;\">%s</span></div>", h.current_reset_desc);
        
        if (h.prev_uptime_sec > 0) {
            client.printf("<div class=\"stat\"><span>前回の連続稼働時間</span><span class=\"stat-val\">%luh %lum %lus</span></div>", (unsigned long)prev_h, (unsigned long)prev_m, (unsigned long)prev_s);
            client.printf("<div class=\"stat\"><span>直前の実行コマンド</span><span class=\"stat-val\">0x%04X (%s)</span></div>", h.prev_last_cmd_id, h.prev_last_cmd_name);
        }

        if (h.has_coredump) {
            client.printf("<div class=\"alert-box\" style=\"border-color:var(--red);background:#450a0a;color:#fca5a5;\">"
                          "<strong>🚨 クラッシュダンプ検出!</strong><br>"
                          "Task: %s | PC: 0x%08lX<br>要因: %s</div>",
                          h.coredump_task, (unsigned long)h.coredump_pc, h.coredump_panic_reason);
        } else if (h.current_reset_reason == ESP_RST_BROWNOUT) {
            client.print("<div class=\"alert-box\">"
                         "<strong>⚡ Brownout 検出 (電源電圧降下)</strong><br>"
                         "2.4GHz RF の高出力送信時に電圧が一時降下しました。電源付き USB ハブまたは高品質ケーブルへの接続を推奨します。</div>");
        } else if (h.current_reset_reason == ESP_RST_TASK_WDT) {
            client.print("<div class=\"alert-box\">"
                         "<strong>⏱️ Task Watchdog 検出</strong><br>"
                         "タスクが CPU を占有しタイムアウトしました。</div>");
        }

        client.print("</div></div>");

        client.print("<div class=\"card\">"
                     "<div style=\"display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--border);padding-bottom:6px;margin-bottom:10px;\">"
                     "<h2 style=\"border:none;margin:0;padding:0;\">直近イベントログ (Recent Logs)</h2>"
                     "<a class=\"btn\" href=\"/logs\" target=\"_blank\">RAW ログ表示</a></div>"
                     "<div class=\"logs\">");
        
        int log_cnt = Diagnostics::get_log_count();
        if (log_cnt == 0) {
            client.print("ログはまだありません。\n");
        } else {
            for (int i = 0; i < log_cnt; i++) {
                client.println(Diagnostics::get_log_entry(i));
            }
        }
        client.print("</div></div></div></body></html>\r\n");
    } else {
        // Unknown endpoint (e.g. /favicon.ico) - quick 404 with no body
        client.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    }
    client.stop();
}

void diagWebTask(void* pvParameters) {
    while (1) {
        if (WiFi.status() == WL_CONNECTED && diagServerStarted) {
            handleDiagWeb();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ==========================================
// Setup and Loop
// ==========================================

extern "C" void app_main() {
    initArduino();
    Diagnostics::init();
    Serial.begin(115200);
    
    client_mutex = xSemaphoreCreateMutex();
    
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(50);
    u8g2.begin();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "Booting...");
    u8g2.sendBuffer();

    #if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    esp_coex_wifi_i154_enable();
    #endif

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    Serial.print("Connecting to Wi-Fi");
    int wifiWaitCount = 0;
    while (WiFi.status() != WL_CONNECTED && wifiWaitCount < 40) {
        delay(500);
        Serial.print(".");
        wifiWaitCount++;
    }
    Serial.println();
    
    // Start Zigbee App in its own task
    xTaskCreate([](void* arg) {
        ESP_LOGI("MAIN", "Zigbee App task starting...");
        esp_err_t err = app::start();
        if (err != ESP_OK) {
            ESP_LOGE("MAIN", "app::start failed: %s", esp_err_to_name(err));
        }
        vTaskDelete(NULL);
    }, "zb_app_task", 8192, NULL, 5, NULL);

    // Start TCP Bridge in dedicated high-priority task (Priority 6) with 8KB stack
    xTaskCreate(bridgeTask, "bridge_task", 8192, NULL, 6, NULL);

    // Start HTTP Web Diagnostics in dedicated low-priority task (Priority 2) with 4KB stack
    xTaskCreate(diagWebTask, "diag_web_task", 4096, NULL, 2, NULL);

    // Initial display update
    updateDisplay();

    bool servicesStarted = false;

    // Main Loop (Handles UI, LED, OTA)
    while (1) {
        if (WiFi.status() == WL_CONNECTED) {
            if (!servicesStarted) {
                WiFi.setSleep(false);
                esp_wifi_set_ps(WIFI_PS_NONE);
                #if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
                esp_err_t coex_err = esp_coex_wifi_i154_enable();
                Serial.printf("esp_coex_wifi_i154_enable: %s\n", esp_err_to_name(coex_err));
                #endif

                deviceIP = WiFi.localIP().toString();
                Serial.printf("Wi-Fi connected. IP: %s, Channel: %d, RSSI: %d\n",
                    deviceIP.c_str(), WiFi.channel(), WiFi.RSSI());
                
                ArduinoOTA.setHostname("esp32c6-z2m-ncp");
                ArduinoOTA.setPassword(OTA_PASSWORD);
                ArduinoOTA.onStart([]() {
                    isOtaUpdating = true;
                    Serial.println("OTA Start");
                    u8g2.clearBuffer();
                    u8g2.setFont(u8g2_font_7x14_tf);
                    u8g2.drawStr(10, 24, "OTA UPDATING...");
                    u8g2.drawFrame(10, 34, 108, 12);
                    u8g2.sendBuffer();
                });
                ArduinoOTA.onEnd([]() {
                    Serial.println("\nOTA End");
                    u8g2.clearBuffer();
                    u8g2.setFont(u8g2_font_7x14_tf);
                    u8g2.drawStr(20, 36, "OTA COMPLETE!");
                    u8g2.sendBuffer();
                    isOtaUpdating = false;
                });
                ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
                    unsigned int percent = (progress / (total / 100));
                    Serial.printf("Progress: %u%%\r", percent);
                    u8g2.drawBox(12, 36, (104 * percent) / 100, 8);
                    u8g2.sendBuffer();
                });
                ArduinoOTA.onError([](ota_error_t error) {
                    isOtaUpdating = false;
                    Serial.printf("Error[%u]\n", error);
                });
                ArduinoOTA.begin();
                
                server.begin();
                server.setNoDelay(true);
                Serial.printf("TCP Server started on port %d\n", Z2M_TCP_PORT);
                
                diagServer.begin();
                diagServer.setNoDelay(true);
                diagServerStarted = true;
                Serial.println("HTTP Diagnostics Server started on port 80");
                
                Diagnostics::log_event("Wi-Fi connected: %s (Ch %d, RSSI %d)", deviceIP.c_str(), WiFi.channel(), WiFi.RSSI());
                Diagnostics::log_event("HTTP Diag Server started on port 80");
                
                servicesStarted = true;
                z2mServerStarted = true;
            }
            ArduinoOTA.handle();
        } else {
            if (servicesStarted) {
                servicesStarted = false;
                z2mServerStarted = false;
                diagServerStarted = false;
                Diagnostics::log_event("Wi-Fi disconnected");
                Serial.println("Wi-Fi disconnected.");
            }
            static unsigned long lastWifiRetry = 0;
            if (millis() - lastWifiRetry > 5000) {
                Serial.println("[WIFI] Auto-reconnecting to Wi-Fi...");
                WiFi.disconnect();
                WiFi.begin(WIFI_SSID, WIFI_PASS);
                lastWifiRetry = millis();
            }
        }
        
        handleLED();
        handlePixelShift();
        
        static unsigned long lastHealthUpdate = 0;
        if (millis() - lastHealthUpdate > 1000) {
            Diagnostics::update_health();
            lastHealthUpdate = millis();
        }
        
        static unsigned long lastDisplayUpdate = 0;
        if (millis() - lastDisplayUpdate > 500) {
            updateDisplay();
            lastDisplayUpdate = millis();
        }
        
        static unsigned long lastAliveLog = 0;
        if (millis() - lastAliveLog > 5000) {
            Serial.printf("[SYSTEM] Uptime: %lus, FreeHeap: %lu, Wi-Fi: %d, IP: %s, Ch: %d, RSSI: %d, Client: %d\n",
                millis() / 1000, (unsigned long)esp_get_free_heap_size(),
                (int)(WiFi.status() == WL_CONNECTED),
                WiFi.localIP().toString().c_str(),
                WiFi.channel(),
                WiFi.RSSI(),
                (int)(z2mClientConnected ? 1 : 0));
            lastAliveLog = millis();
        }
        
        delay(10);
    }
}
