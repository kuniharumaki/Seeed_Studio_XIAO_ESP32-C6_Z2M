#pragma once

#include <esp_system.h>
#include <stdint.h>
#include <stdbool.h>

struct DiagnosticsHealth {
    uint32_t boot_count;
    esp_reset_reason_t current_reset_reason;
    char current_reset_name[24];
    char current_reset_desc[64];
    
    // Previous run metrics (preserved in RTC RAM across resets)
    esp_reset_reason_t prev_reset_reason;
    char prev_reset_name[24];
    uint32_t prev_uptime_sec;
    uint32_t prev_min_free_heap;
    uint16_t prev_last_cmd_id;
    uint8_t  prev_last_tsn;
    char prev_last_cmd_name[24];
    
    // CoreDump crash information (if retrieved on boot)
    bool has_coredump;
    char coredump_task[16];
    uint32_t coredump_pc;
    char coredump_panic_reason[96];
};

class Diagnostics {
public:
    static void init();
    static void update_health();
    static void update_last_cmd(uint16_t cmd_id, uint8_t tsn, const char* name);
    static void log_event(const char* fmt, ...);
    
    static const DiagnosticsHealth& get_health();
    static int get_log_count();
    static const char* get_log_entry(int index);
    
    static const char* reset_reason_to_name(esp_reset_reason_t reason);
    static const char* reset_reason_to_desc(esp_reset_reason_t reason);
};
