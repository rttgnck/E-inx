/**
 * @file AgentIslandPairing.cpp
 * @brief Definitions for AgentIslandPairing.
 */

#include "state/AgentIslandPairing.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <algorithm>
#include <cctype>

#ifndef SIMULATOR
#include <WiFi.h>
#endif

AgentIslandStore AgentIslandStore::instance;

namespace {

constexpr uint8_t AGENT_ISLAND_FILE_VERSION = 1;

constexpr char AGENT_ISLAND_FILE[] = "/.system/agentisland.bin";

constexpr uint8_t OBFUSCATION_KEY[] = {0x41, 0x67, 0x65, 0x6E, 0x74, 0x49, 0x73, 0x6C, 0x61, 0x6E, 0x64};
constexpr size_t KEY_LENGTH = sizeof(OBFUSCATION_KEY);

std::string percentDecode(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '+') {
      out.push_back(' ');
      continue;
    }
    if (value[i] == '%' && i + 2 < value.size() && isxdigit(static_cast<unsigned char>(value[i + 1])) &&
        isxdigit(static_cast<unsigned char>(value[i + 2]))) {
      const std::string hex = value.substr(i + 1, 2);
      out.push_back(static_cast<char>(strtol(hex.c_str(), nullptr, 16)));
      i += 2;
      continue;
    }
    out.push_back(value[i]);
  }
  return out;
}

/** Pulls one `name=value` out of a query string. Returns empty when absent. */
std::string queryValue(const std::string& query, const std::string& name) {
  size_t cursor = 0;
  while (cursor < query.size()) {
    const size_t end = query.find('&', cursor);
    const std::string pair = query.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
    const size_t equals = pair.find('=');
    if (equals != std::string::npos && pair.compare(0, equals, name) == 0) {
      return percentDecode(pair.substr(equals + 1));
    }
    if (end == std::string::npos) break;
    cursor = end + 1;
  }
  return "";
}

std::string trimmed(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && isspace(static_cast<unsigned char>(value[begin]))) ++begin;
  while (end > begin && isspace(static_cast<unsigned char>(value[end - 1]))) --end;
  return value.substr(begin, end - begin);
}

bool isHex64(const std::string& value) {
  if (value.size() != 64) return false;
  return std::all_of(value.begin(), value.end(),
                     [](const char c) { return isxdigit(static_cast<unsigned char>(c)) != 0; });
}

}  // namespace

void AgentIslandStore::obfuscate(std::string& data) const {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= OBFUSCATION_KEY[i % KEY_LENGTH];
  }
}

bool AgentIslandStore::saveToFile() const {
  SdMan.mkdir("/.system");

  FsFile file;
  if (!SdMan.openFileForWrite("AIS", AGENT_ISLAND_FILE, file)) {
    return false;
  }

  serialization::writePod(file, AGENT_ISLAND_FILE_VERSION);
  serialization::writeString(file, pairing.host);
  serialization::writePod(file, pairing.port);
  serialization::writeString(file, pairing.fingerprint);
  serialization::writeString(file, pairing.deviceId);
  serialization::writeString(file, pairing.resolvedAddress);

  std::string secret = pairing.enrollmentToken;
  obfuscate(secret);
  serialization::writeString(file, secret);

  std::string token = pairing.deviceToken;
  obfuscate(token);
  serialization::writeString(file, token);

  file.close();
  Serial.printf("[%lu] [AIS] Saved Agent Island pairing for %s\n", millis(), pairing.host.c_str());
  return true;
}

bool AgentIslandStore::loadFromFile() {
  loaded = true;

  FsFile file;
  if (!SdMan.openFileForRead("AIS", AGENT_ISLAND_FILE, file)) {
    pairing = AgentIslandPairing{};
    return false;
  }

  uint8_t version = 0;
  serialization::readPod(file, version);
  if (version != AGENT_ISLAND_FILE_VERSION) {
    Serial.printf("[%lu] [AIS] Unknown file version: %u\n", millis(), version);
    file.close();
    pairing = AgentIslandPairing{};
    return false;
  }

  AgentIslandPairing next;
  serialization::readString(file, next.host);
  serialization::readPod(file, next.port);
  serialization::readString(file, next.fingerprint);
  serialization::readString(file, next.deviceId);
  serialization::readString(file, next.resolvedAddress);
  serialization::readString(file, next.enrollmentToken);
  obfuscate(next.enrollmentToken);
  serialization::readString(file, next.deviceToken);
  obfuscate(next.deviceToken);
  file.close();

  pairing = next;
  Serial.printf("[%lu] [AIS] Loaded Agent Island pairing for %s (paired: %s)\n", millis(), pairing.host.c_str(),
                pairing.isPaired() ? "yes" : "no");
  return true;
}

const AgentIslandPairing& AgentIslandStore::get() {
  if (!loaded) {
    loadFromFile();
  }
  return pairing;
}

bool AgentIslandStore::setPairingPayload(const AgentIslandPairing& next) {
  get();
  const std::string keptDeviceId = pairing.deviceId.empty() ? localDeviceId() : pairing.deviceId;
  pairing = next;
  pairing.deviceId = keptDeviceId;
  // A fresh code is a fresh enrollment: the Mac rotates its secret on every
  // successful pair, so any token we already hold belongs to the previous one.
  pairing.deviceToken.clear();
  pairing.resolvedAddress.clear();
  return saveToFile();
}

bool AgentIslandStore::setDeviceToken(const std::string& token) {
  get();
  pairing.deviceToken = token;
  pairing.enrollmentToken.clear();
  return saveToFile();
}

bool AgentIslandStore::setEndpoint(const std::string& host, const uint16_t port) {
  get();
  if (host.empty() || port == 0) return false;
  pairing.host = host;
  pairing.port = port;
  // The remembered address belonged to the old name; make the next connection
  // resolve rather than dial somewhere that is no longer the Mac.
  pairing.resolvedAddress.clear();
  return saveToFile();
}

bool AgentIslandStore::setResolvedAddress(const std::string& address) {
  get();
  if (pairing.resolvedAddress == address) return true;
  pairing.resolvedAddress = address;
  return saveToFile();
}

void AgentIslandStore::forget() {
  pairing = AgentIslandPairing{};
  loaded = true;
  SdMan.remove(AGENT_ISLAND_FILE);
}

bool AgentIslandStore::parsePairingUrl(const std::string& payload, AgentIslandPairing& out) {
  const std::string text = trimmed(payload);
  const size_t queryStart = text.find('?');
  if (queryStart == std::string::npos) return false;

  const std::string query = text.substr(queryStart + 1);
  const std::string url = trimmed(queryValue(query, "url"));
  const std::string token = trimmed(queryValue(query, "token"));
  const std::string pin = trimmed(queryValue(query, "pin"));
  if (url.empty() || token.empty() || !isHex64(pin)) return false;

  // https://host[:port] — the scheme is fixed, so anything else is a payload we
  // do not understand rather than one we should guess at.
  const std::string scheme = "https://";
  if (url.compare(0, scheme.size(), scheme) != 0) return false;
  std::string authority = url.substr(scheme.size());
  const size_t slash = authority.find('/');
  if (slash != std::string::npos) authority = authority.substr(0, slash);
  if (authority.empty()) return false;

  AgentIslandPairing parsed;
  const size_t colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(']') == std::string::npos) {
    parsed.host = authority.substr(0, colon);
    const long port = strtol(authority.c_str() + colon + 1, nullptr, 10);
    if (port <= 0 || port > 65535) return false;
    parsed.port = static_cast<uint16_t>(port);
  } else {
    parsed.host = authority;
  }
  if (parsed.host.empty()) return false;

  parsed.enrollmentToken = token;
  parsed.fingerprint = pin;
  std::transform(parsed.fingerprint.begin(), parsed.fingerprint.end(), parsed.fingerprint.begin(),
                 [](const unsigned char c) { return static_cast<char>(tolower(c)); });

  out = parsed;
  return true;
}

std::string AgentIslandStore::localDeviceId() {
#ifdef SIMULATOR
  return "einx-simulator";
#else
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "einx-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buffer);
#endif
}
