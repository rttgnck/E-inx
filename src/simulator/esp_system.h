#pragma once

/**
 * @file esp_system.h
 * @brief Simulator esp_system.h (replaces the simulator library's copy; see
 * scripts/simulator_lib_overrides.py).
 *
 * The library reports ESP_RST_POWERON. E-inx reads a power-on reset with no
 * wakeup cause as a power-button wake, finds the button not held (no pin is
 * ever held here) and goes straight back to sleep. ESP_RST_UNKNOWN is what the
 * X3 reports after a USB flash, which boots to the UI.
 */

#ifdef SIMULATOR

#include <cstdint>

typedef enum {
  ESP_RST_UNKNOWN,
  ESP_RST_POWERON,
  ESP_RST_EXT,
  ESP_RST_SW,
  ESP_RST_PANIC,
  ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT,
  ESP_RST_WDT,
  ESP_RST_DEEPSLEEP,
  ESP_RST_BROWNOUT,
  ESP_RST_SDIO,
} esp_reset_reason_t;

inline void esp_restart() {}
inline uint32_t esp_get_free_heap_size() { return 1000000; }
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_UNKNOWN; }

#else
#error "src/simulator/esp_system.h is only for simulator builds"
#endif
