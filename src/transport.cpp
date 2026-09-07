#include "transport.h"
#include "app.h"
#include "protocol.h"
#include <WiFi.h>

// Globals from main.cpp
extern WiFiClient z2mClient;
extern unsigned long bridgeRxCount;
extern unsigned long lastPulseTime;

extern SemaphoreHandle_t client_mutex;

esp_err_t transport::start() {
    return ESP_OK;
}

esp_err_t transport::process_output(void* buffer, uint16_t size) {
    if (!buffer) return ESP_ERR_INVALID_ARG;
    if (size == 0) {
        free(buffer);
        return ESP_OK;
    }
    // Forward from host to NCP via protocol parser
    esp_err_t ret = protocol::on_receive_data(static_cast<uint8_t*>(buffer), size);
    free(buffer);
    return ret;
}

esp_err_t transport::process_input(void* buffer, uint16_t size) {
    if (!buffer) return ESP_ERR_INVALID_ARG;
    if (size == 0) {
        free(buffer);
        return ESP_OK;
    }
    
    if (client_mutex) {
        xSemaphoreTake(client_mutex, portMAX_DELAY);
    }
    
    // Write directly to TCP socket
    if (z2mClient && z2mClient.connected()) {
        size_t written = z2mClient.write(static_cast<const uint8_t*>(buffer), size);
        bridgeRxCount += size;
        lastPulseTime = millis();
        
        Serial.printf("[TX->Z2M] (%d/%d bytes written): ", (int)written, (int)size);
        uint8_t* b = static_cast<uint8_t*>(buffer);
        for(size_t i=0; i<size; i++) Serial.printf("%02X ", b[i]);
        Serial.println();
    } else {
        Serial.printf("[TX->Z2M] dropped (%d bytes) - z2mClient not connected\n", (int)size);
    }
    
    if (client_mutex) {
        xSemaphoreGive(client_mutex);
    }
    
    free(buffer);
    return ESP_OK;
}

esp_err_t transport::send(void* data, uint16_t size) {
    if (!data) return ESP_ERR_INVALID_ARG;
    if (size == 0) {
        free(data);
        return ESP_OK;
    }
    // Queue event to app task to be processed as EVENT_INPUT (NCP -> Host)
    app::ctx_t ncp_event = {
        .event = app::EVENT_INPUT,
        .size = size,
        .buf_ptr = static_cast<uint8_t*>(data)
    };
    return app::send_event(ncp_event);
}
