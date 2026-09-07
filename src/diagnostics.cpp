#include "diagnostics.h"
#include <esp_core_dump.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <esp_attr.h>
#include <nvs.h>
#include <nvs_flash.h>

#define RTC_MAGIC 0x5A424F53 // "ZBOS"

struct RtcHealthData {
    uint32_t magic;
    uint32_t boot_count;
    esp_reset_reason_t last_reset_reason;
    uint32_t uptime_sec;
    uint32_t min_free_heap;
    int8_t   wifi_rssi;
    uint16_t last_cmd_id;
    uint8_t  last_tsn;
    char     last_cmd_name[24];
};

RTC_NOINIT_ATTR static RtcHealthData s_rtc_data;

static DiagnosticsHealth s_health;
static SemaphoreHandle_t s_diag_mutex = NULL;

// In-memory log ring buffer (40 entries x 96 chars = ~3.8 KB)
#define LOG_BUFFER_SIZE 40
#define LOG_ENTRY_LEN   96
static char s_log_buffer[LOG_BUFFER_SIZE][LOG_ENTRY_LEN];
static int s_log_head = 0;
static int s_log_count = 0;

const char* Diagnostics::reset_reason_to_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXT_PIN";
        case ESP_RST_SW:         return "SW_RESET";
        case ESP_RST_PANIC:      return "PANIC_CRASH";
        case ESP_RST_INT_WDT:    return "INT_WDT";
        case ESP_RST_TASK_WDT:   return "TASK_WDT";
        case ESP_RST_WDT:        return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        case ESP_RST_USB:        return "USB_RESET";
        case ESP_RST_JTAG:       return "JTAG";
        case ESP_RST_EFUSE:      return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        default:                 return "UNKNOWN";
    }
}

const char* Diagnostics::reset_reason_to_desc(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:    return "Normal Power-On";
        case ESP_RST_EXT:        return "Reset by external pin";
        case ESP_RST_SW:         return "Software reset via esp_restart()";
        case ESP_RST_PANIC:      return "Software panic / exception crash";
        case ESP_RST_INT_WDT:    return "Interrupt watchdog timeout (ISR hung)";
        case ESP_RST_TASK_WDT:   return "Task watchdog timeout (Task starved/blocked)";
        case ESP_RST_WDT:        return "RTC or other watchdog reset";
        case ESP_RST_DEEPSLEEP:  return "Wakeup from deep sleep";
        case ESP_RST_BROWNOUT:   return "Brownout reset (Power supply voltage dip)";
        case ESP_RST_SDIO:       return "Reset over SDIO";
        case ESP_RST_USB:        return "Reset by USB CDC/JTAG (Flashing / Serial Tool)";
        case ESP_RST_JTAG:       return "Reset by JTAG debugger";
        case ESP_RST_EFUSE:      return "Reset due to eFuse error";
        case ESP_RST_PWR_GLITCH: return "Reset due to power glitch";
        case ESP_RST_CPU_LOCKUP: return "CPU Lockup (Double exception / hard fault)";
        default:                 return "Unknown reset reason";
    }
}

void Diagnostics::init() {
    if (!s_diag_mutex) {
        s_diag_mutex = xSemaphoreCreateMutex();
    }
    
    esp_reset_reason_t reason = esp_reset_reason();
    s_health.current_reset_reason = reason;
    strncpy(s_health.current_reset_name, reset_reason_to_name(reason), sizeof(s_health.current_reset_name) - 1);
    strncpy(s_health.current_reset_desc, reset_reason_to_desc(reason), sizeof(s_health.current_reset_desc) - 1);
    
    if (s_rtc_data.magic != RTC_MAGIC) {
        // Cold boot: initialize RTC record
        s_rtc_data.magic = RTC_MAGIC;
        s_rtc_data.boot_count = 1;
        s_rtc_data.last_reset_reason = reason;
        s_rtc_data.uptime_sec = 0;
        s_rtc_data.min_free_heap = esp_get_free_heap_size();
        s_rtc_data.wifi_rssi = 0;
        s_rtc_data.last_cmd_id = 0;
        s_rtc_data.last_tsn = 0;
        s_rtc_data.last_cmd_name[0] = '\0';
        
        s_health.boot_count = 1;
        s_health.prev_reset_reason = ESP_RST_UNKNOWN;
        strncpy(s_health.prev_reset_name, "NONE (Cold Boot)", sizeof(s_health.prev_reset_name) - 1);
        s_health.prev_uptime_sec = 0;
        s_health.prev_min_free_heap = 0;
        s_health.prev_last_cmd_id = 0;
        s_health.prev_last_tsn = 0;
        s_health.prev_last_cmd_name[0] = '\0';
    } else {
        // Warm boot: preserve previous run's metrics
        s_health.boot_count = ++s_rtc_data.boot_count;
        s_health.prev_reset_reason = s_rtc_data.last_reset_reason;
        strncpy(s_health.prev_reset_name, reset_reason_to_name(s_rtc_data.last_reset_reason), sizeof(s_health.prev_reset_name) - 1);
        s_health.prev_uptime_sec = s_rtc_data.uptime_sec;
        s_health.prev_min_free_heap = s_rtc_data.min_free_heap;
        s_health.prev_last_cmd_id = s_rtc_data.last_cmd_id;
        s_health.prev_last_tsn = s_rtc_data.last_tsn;
        strncpy(s_health.prev_last_cmd_name, s_rtc_data.last_cmd_name, sizeof(s_health.prev_last_cmd_name) - 1);
        
        // Reset current session tracking
        s_rtc_data.last_reset_reason = reason;
        s_rtc_data.uptime_sec = 0;
        s_rtc_data.min_free_heap = esp_get_free_heap_size();
        s_rtc_data.last_cmd_id = 0;
        s_rtc_data.last_tsn = 0;
        s_rtc_data.last_cmd_name[0] = '\0';
    }

    // CoreDump extraction (if enabled in sdkconfig)
    s_health.has_coredump = false;
    #if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t summary;
    if (esp_core_dump_get_summary(&summary) == ESP_OK) {
        s_health.has_coredump = true;
        strncpy(s_health.coredump_task, summary.exc_task, sizeof(s_health.coredump_task) - 1);
        s_health.coredump_task[sizeof(s_health.coredump_task) - 1] = '\0';
        s_health.coredump_pc = (uint32_t)summary.exc_pc;
        
        char panic_buf[96] = {0};
        if (esp_core_dump_get_panic_reason(panic_buf, sizeof(panic_buf)) == ESP_OK && strlen(panic_buf) > 0) {
            strncpy(s_health.coredump_panic_reason, panic_buf, sizeof(s_health.coredump_panic_reason) - 1);
        } else {
#if defined(__riscv)
            snprintf(s_health.coredump_panic_reason, sizeof(s_health.coredump_panic_reason), "mcause: 0x%lx", (unsigned long)summary.ex_info.mcause);
#else
            snprintf(s_health.coredump_panic_reason, sizeof(s_health.coredump_panic_reason), "cause: 0x%lx", (unsigned long)summary.ex_info.cause);
#endif
        }
        s_health.coredump_panic_reason[sizeof(s_health.coredump_panic_reason) - 1] = '\0';
        
        ESP_LOGW("DIAG", "CoreDump found! Crashed Task: %s, PC: 0x%08lx, Reason: %s",
                 s_health.coredump_task, (unsigned long)s_health.coredump_pc, s_health.coredump_panic_reason);
        esp_core_dump_image_erase();
    }
    #endif

    // NVS Persistence for abnormal reset tracking (survives full power-off)
    nvs_handle_t nvs_h;
    if (nvs_open("diag", NVS_READWRITE, &nvs_h) == ESP_OK) {
        if (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_BROWNOUT) {
            // Save abnormal reset metrics to Flash
            nvs_set_u8(nvs_h, "last_reason", (uint8_t)reason);
            nvs_set_u32(nvs_h, "last_uptime", s_health.prev_uptime_sec);
            nvs_set_u16(nvs_h, "last_cmd", s_health.prev_last_cmd_id);
            nvs_set_u8(nvs_h, "last_tsn", s_health.prev_last_tsn);
            nvs_set_str(nvs_h, "last_cmd_name", s_health.prev_last_cmd_name);
            nvs_commit(nvs_h);
        } else if (s_health.prev_uptime_sec == 0) {
            // If cold boot / uninitialized, restore previous crash information from NVS
            uint8_t nvs_reason = 0;
            if (nvs_get_u8(nvs_h, "last_reason", &nvs_reason) == ESP_OK && nvs_reason != 0) {
                s_health.prev_reset_reason = (esp_reset_reason_t)nvs_reason;
                strncpy(s_health.prev_reset_name, reset_reason_to_name((esp_reset_reason_t)nvs_reason), sizeof(s_health.prev_reset_name) - 1);
                nvs_get_u32(nvs_h, "last_uptime", &s_health.prev_uptime_sec);
                nvs_get_u16(nvs_h, "last_cmd", &s_health.prev_last_cmd_id);
                nvs_get_u8(nvs_h, "last_tsn", &s_health.prev_last_tsn);
                size_t name_len = sizeof(s_health.prev_last_cmd_name);
                nvs_get_str(nvs_h, "last_cmd_name", s_health.prev_last_cmd_name, &name_len);
            }
        }
        nvs_close(nvs_h);
    }

    log_event("Boot #%lu, Reason: %s (%s)", (unsigned long)s_health.boot_count, s_health.current_reset_name, s_health.current_reset_desc);
    if (s_health.prev_uptime_sec > 0) {
        log_event("Previous Uptime: %lus, LastCmd: 0x%04x (TSN %u: %s)",
                  (unsigned long)s_health.prev_uptime_sec, s_health.prev_last_cmd_id, s_health.prev_last_tsn, s_health.prev_last_cmd_name);
    }
    if (s_health.has_coredump) {
        log_event("CRASH! Task: %s, PC: 0x%08lx, %s", s_health.coredump_task, (unsigned long)s_health.coredump_pc, s_health.coredump_panic_reason);
    }
}

void Diagnostics::update_health() {
    uint32_t now_sec = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t free_h = esp_get_free_heap_size();
    uint32_t min_h = esp_get_minimum_free_heap_size();
    
    s_rtc_data.uptime_sec = now_sec;
    s_rtc_data.min_free_heap = min_h;
}

void Diagnostics::update_last_cmd(uint16_t cmd_id, uint8_t tsn, const char* name) {
    s_rtc_data.last_cmd_id = cmd_id;
    s_rtc_data.last_tsn = tsn;
    if (name) {
        strncpy(s_rtc_data.last_cmd_name, name, sizeof(s_rtc_data.last_cmd_name) - 1);
        s_rtc_data.last_cmd_name[sizeof(s_rtc_data.last_cmd_name) - 1] = '\0';
    }
}

void Diagnostics::log_event(const char* fmt, ...) {
    char entry[LOG_ENTRY_LEN];
    uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
    int offset = snprintf(entry, sizeof(entry), "[+%lus] ", (unsigned long)sec);
    
    va_list args;
    va_start(args, fmt);
    vsnprintf(entry + offset, sizeof(entry) - offset, fmt, args);
    va_end(args);
    
    if (s_diag_mutex && xSemaphoreTake(s_diag_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        strncpy(s_log_buffer[s_log_head], entry, LOG_ENTRY_LEN - 1);
        s_log_buffer[s_log_head][LOG_ENTRY_LEN - 1] = '\0';
        s_log_head = (s_log_head + 1) % LOG_BUFFER_SIZE;
        if (s_log_count < LOG_BUFFER_SIZE) {
            s_log_count++;
        }
        xSemaphoreGive(s_diag_mutex);
    }
}

const DiagnosticsHealth& Diagnostics::get_health() {
    return s_health;
}

int Diagnostics::get_log_count() {
    return s_log_count;
}

const char* Diagnostics::get_log_entry(int index) {
    if (index < 0 || index >= s_log_count) return "";
    int start_idx;
    if (s_log_count < LOG_BUFFER_SIZE) {
        start_idx = 0;
    } else {
        start_idx = s_log_head;
    }
    int actual_idx = (start_idx + index) % LOG_BUFFER_SIZE;
    return s_log_buffer[actual_idx];
}
