#pragma once
// The real header pulls in esp_err.h; mirror the surface the sketch uses so a
// host build type-checks the return value the same way the target does.
typedef int esp_err_t;
#define ESP_OK                0
#define ESP_ERR_INVALID_STATE 0x103

inline esp_err_t esp_task_wdt_init(int, bool) { return ESP_OK; }
inline esp_err_t esp_task_wdt_add(void*)      { return ESP_OK; }
inline esp_err_t esp_task_wdt_reset()         { return ESP_OK; }
inline esp_err_t esp_task_wdt_delete(void*)   { return ESP_OK; }
inline esp_err_t esp_task_wdt_deinit()        { return ESP_OK; }
