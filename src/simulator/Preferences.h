#pragma once

/**
 * @file Preferences.h
 * @brief In-memory stand-in for the ESP32 NVS Preferences API.
 *
 * Replaces the simulator library's Preferences.h (see
 * scripts/simulator_lib_overrides.py), which only has the 64-bit calls.
 * Values last for one simulator run; nothing is written to disk.
 */

#ifdef SIMULATOR

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

class Preferences {
 public:
  bool begin(const char* name, bool readOnly = false) {
    ns = name ? name : "";
    (void)readOnly;
    return true;
  }
  void end() {}

  uint32_t getUInt(const char* key, uint32_t defaultValue = 0) {
    const auto it = store().find(ns + "/" + key);
    return it == store().end() ? defaultValue : static_cast<uint32_t>(it->second);
  }
  size_t putUInt(const char* key, uint32_t value) {
    store()[ns + "/" + key] = value;
    return sizeof(value);
  }
  int64_t getLong64(const char* key, int64_t defaultValue = 0) {
    const auto it = store().find(ns + "/" + key);
    return it == store().end() ? defaultValue : static_cast<int64_t>(it->second);
  }
  size_t putLong64(const char* key, int64_t value) {
    store()[ns + "/" + key] = static_cast<uint64_t>(value);
    return sizeof(value);
  }
  bool remove(const char* key) { return store().erase(ns + "/" + key) > 0; }
  bool clear() {
    for (auto it = store().begin(); it != store().end();) {
      it = it->first.rfind(ns + "/", 0) == 0 ? store().erase(it) : std::next(it);
    }
    return true;
  }

 private:
  static std::map<std::string, uint64_t>& store() {
    static std::map<std::string, uint64_t> values;
    return values;
  }
  std::string ns;
};

#else
#error "src/simulator/Preferences.h is only for simulator builds"
#endif
