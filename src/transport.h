#pragma once
#include <cstdint>
#include <esp_err.h>

class transport {
public:
    transport(const transport&) = delete;
    transport& operator=(const transport&) = delete;
    transport(transport&&) = delete;
    transport& operator=(transport&&) = delete;
    
    // Start is a no-op now since we handle receiving in main.cpp
    static esp_err_t start();
    
    // Called by app when data is received from host (Host -> NCP)
    // Routes to protocol::on_receive_data
    static esp_err_t process_output(void* buffer, uint16_t size);
    
    // Called by app when data is ready to send to host (NCP -> Host)
    // Writes to TCP socket
    static esp_err_t process_input(void* buffer, uint16_t size);
    
    // Called by protocol to queue data to host
    static esp_err_t send(void* data, uint16_t size);
private:
    transport() {}
    ~transport() {}
};
