#pragma once

/**
 * @file AgentIslandPairing.h
 * @brief Public interface and types for AgentIslandPairing.
 */

#include <cstdint>
#include <string>

/**
 * What the device needs to talk to a Mac running Agent Island.
 *
 * All of it except `deviceId` and `deviceToken` comes out of the QR payload the
 * Mac shows in Settings: `agentisland://pair?v=1&url=…&token=…&pin=…`. The X3 has
 * no camera, so the payload arrives as a pasted string through the web manager
 * (LocalServer's /api/agentisland) rather than through a scan.
 */
struct AgentIslandPairing {
  std::string host;              ///< Hostname or IP from the pairing URL, e.g. "rttgnck-mbp.local".
  uint16_t port = 47124;         ///< The companion listener. The hook bridge on 47123 is loopback-only.
  std::string fingerprint;       ///< Lowercase hex SHA-256 of the server's DER certificate.
  std::string enrollmentToken;   ///< One-time pairing secret; cleared once exchanged for a device token.
  std::string deviceId;          ///< This device's identity on the Mac. Generated once, never changes.
  std::string deviceToken;       ///< Long-lived bearer token issued by POST /api/pair.
  std::string resolvedAddress;   ///< Last IP that answered, so a reconnect can skip resolution.

  bool hasEndpoint() const { return !host.empty() && !fingerprint.empty(); }
  bool isPaired() const { return hasEndpoint() && !deviceToken.empty(); }
  bool awaitingEnrollment() const { return hasEndpoint() && deviceToken.empty() && !enrollmentToken.empty(); }
};

/**
 * Singleton store for the Agent Island pairing, kept in /sd/.system/agentisland.bin
 * with the same XOR obfuscation the WiFi and OPDS stores use for their secrets.
 * That is not encryption — it keeps the bearer token off the screen of anyone who
 * mounts the card and greps it, and nothing more.
 */
class AgentIslandStore {
 private:
  static AgentIslandStore instance;
  AgentIslandPairing pairing;
  bool loaded = false;

  AgentIslandStore() = default;

  void obfuscate(std::string& data) const;

 public:
  AgentIslandStore(const AgentIslandStore&) = delete;
  AgentIslandStore& operator=(const AgentIslandStore&) = delete;

  static AgentIslandStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();
  /** Loads once per boot; later calls are free. */
  const AgentIslandPairing& get();

  /** Replaces the endpoint and clears any device token, because a new code means a new enrollment. */
  bool setPairingPayload(const AgentIslandPairing& next);
  /** Records the token POST /api/pair handed back, and drops the spent enrollment secret. */
  bool setDeviceToken(const std::string& token);
  /** Remembers the address that answered, so the next session can skip mDNS and the scan. */
  bool setResolvedAddress(const std::string& address);
  void forget();

  /**
   * Parses `agentisland://pair?v=1&url=https://host:port&token=…&pin=…`.
   * Returns false if any of url, token or pin is missing or malformed; `out` is
   * untouched in that case. A bare `https://host:port` is not accepted — the pin
   * is what makes the connection safe, so a payload without one is not a pairing.
   */
  static bool parsePairingUrl(const std::string& payload, AgentIslandPairing& out);

  /** A stable per-device identifier derived from the WiFi MAC. */
  static std::string localDeviceId();
};

#define AGENT_ISLAND_STORE AgentIslandStore::getInstance()
