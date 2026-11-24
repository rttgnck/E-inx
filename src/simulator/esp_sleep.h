#pragma once

/**
 * @file esp_sleep.h
 * @brief Simulator stand-in for ESP-IDF's sleep and reset-reason API.
 *
 * Every simulator start has no wakeup cause; the reset reason comes from
 * esp_system.h. The library's sleep screen relaunches the process on a key
 * press and reports that through gpio.getWakeupReason() instead.
 * RTC_DATA_ATTR / RTC_NOINIT_ATTR variables are ordinary globals here.
 */

#ifdef SIMULATOR

#include <esp_system.h>

#include <cstdint>

#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR
#endif
#ifndef RTC_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#endif

typedef enum {
  ESP_SLEEP_WAKEUP_UNDEFINED = 0,
  ESP_SLEEP_WAKEUP_ALL,
  ESP_SLEEP_WAKEUP_EXT0,
  ESP_SLEEP_WAKEUP_EXT1,
  ESP_SLEEP_WAKEUP_TIMER,
  ESP_SLEEP_WAKEUP_TOUCHPAD,
  ESP_SLEEP_WAKEUP_ULP,
  ESP_SLEEP_WAKEUP_GPIO,
  ESP_SLEEP_WAKEUP_UART,
} esp_sleep_wakeup_cause_t;

inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return ESP_SLEEP_WAKEUP_UNDEFINED; }
inline int esp_sleep_enable_timer_wakeup(uint64_t) { return 0; }
inline uint64_t esp_sleep_get_gpio_wakeup_status() { return 0; }

#else
#error "src/simulator/esp_sleep.h is only for simulator builds"
#endif
