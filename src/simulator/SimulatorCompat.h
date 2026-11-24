#pragma once

#ifdef SIMULATOR

#ifdef __cplusplus
#include <chrono>
#include <cstdint>
#include <cstdlib>
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned int;
#else
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
#endif

#ifndef pdPASS
#define pdPASS 1
#endif

#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) (ms)
#endif

#ifndef SNTP_SYNC_STATUS_RESET
#define SNTP_SYNC_STATUS_RESET 0
#endif

#ifdef __cplusplus
inline void sntp_set_sync_status(int) {}
#endif

#ifndef vSemaphoreDelete
#define vSemaphoreDelete(sem) delete (sem)
#endif

#ifdef __cplusplus
inline TickType_t xTaskGetTickCount() {
  using namespace std::chrono;
  static const steady_clock::time_point start = steady_clock::now();
  return static_cast<TickType_t>(duration_cast<milliseconds>(steady_clock::now() - start).count());
}

inline uint32_t esp_random() {
  static uint32_t state = 0x9e3779b9U;
  state = state * 1664525U + 1013904223U + xTaskGetTickCount();
  return state;
}
#endif

#ifdef __cplusplus
// Arduino calls E-inx uses that the simulator library's Arduino.h lacks.
// Every pin reads HIGH, so the power button is never seen as held at boot.
#ifndef LOW
#define LOW 0x0
#endif
#ifndef HIGH
#define HIGH 0x1
#endif
#ifndef INPUT_PULLUP
#define INPUT_PULLUP 0x05
#endif
inline void pinMode(uint8_t, uint8_t) {}
inline int digitalRead(uint8_t) { return HIGH; }
inline void randomSeed(unsigned long seed) { std::srand(static_cast<unsigned>(seed)); }
inline long random(long minValue, long maxValue) {
  return maxValue <= minValue ? minValue : minValue + std::rand() % (maxValue - minValue);
}
#endif

#ifndef MANUAL_REFRESH
#define MANUAL_REFRESH FAST_REFRESH
#endif

#ifndef STRONG_FAST_REFRESH
#define STRONG_FAST_REFRESH FAST_REFRESH
#endif

#endif
