/**
 * @file AgentIslandClient.cpp
 * @brief Definitions for AgentIslandClient.
 */

#include "network/AgentIslandClient.h"

#include <HardwareSerial.h>

#include <algorithm>
#include <cstring>

#include "state/NetworkCredential.h"

#ifndef SIMULATOR

#include <ESPmDNS.h>
#include <WiFi.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <unistd.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>

namespace {

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t TLS_CONNECT_TIMEOUT_MS = 4000;
constexpr uint32_t TLS_IO_TIMEOUT_MS = 8000;
/** The sweep is 253 connects; anything generous here turns it into minutes. */
constexpr uint32_t SCAN_CONNECT_TIMEOUT_MS = 120;
/**
 * A hard ceiling on one request. mbedtls_net_recv() reports an expired
 * SO_RCVTIMEO as MBEDTLS_ERR_SSL_WANT_READ, which is indistinguishable from
 * "call me again" — so a stalled peer would spin the retry loops forever
 * without a deadline of our own.
 */
constexpr uint32_t REQUEST_DEADLINE_MS = 20000;
/** A state snapshot with a dozen sessions runs to a few KB. Past this we are being fed something else. */
constexpr size_t MAX_RESPONSE_BYTES = 24 * 1024;

/**
 * TCP connect with a deadline. mbedtls_net_connect() blocks on lwIP's own SYN
 * retry schedule, which is fine for one known-good host and useless for a /24
 * sweep, so the socket is ours and mbedTLS is handed the descriptor.
 */
int connectWithTimeout(const std::string& host, const uint16_t port, const uint32_t timeoutMs) {
  struct sockaddr_in target = {};
  target.sin_family = AF_INET;
  target.sin_port = htons(port);

  IPAddress parsed;
  if (parsed.fromString(host.c_str())) {
    target.sin_addr.s_addr = static_cast<uint32_t>(parsed);
  } else {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* info = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &info) != 0 || info == nullptr) {
      if (info) freeaddrinfo(info);
      return -1;
    }
    target.sin_addr = reinterpret_cast<struct sockaddr_in*>(info->ai_addr)->sin_addr;
    freeaddrinfo(info);
  }

  const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) return -1;

  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  if (connect(sock, reinterpret_cast<struct sockaddr*>(&target), sizeof(target)) < 0 && errno != EINPROGRESS) {
    close(sock);
    return -1;
  }

  fd_set writable;
  FD_ZERO(&writable);
  FD_SET(sock, &writable);
  struct timeval deadline = {};
  deadline.tv_sec = static_cast<time_t>(timeoutMs / 1000);
  deadline.tv_usec = static_cast<suseconds_t>((timeoutMs % 1000) * 1000);
  if (select(sock + 1, nullptr, &writable, nullptr, &deadline) <= 0) {
    close(sock);
    return -1;
  }

  int error = 0;
  socklen_t errorLength = sizeof(error);
  if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &errorLength) < 0 || error != 0) {
    close(sock);
    return -1;
  }

  fcntl(sock, F_SETFL, flags);
  struct timeval io = {};
  io.tv_sec = static_cast<time_t>(TLS_IO_TIMEOUT_MS / 1000);
  io.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof(io));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof(io));
  return sock;
}

std::string hexDigest(const uint8_t digest[32]) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (int i = 0; i < 32; ++i) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0F]);
  }
  return out;
}

/** Constant-time compare, so a wrong pin does not leak how much of it was right. */
bool digestsEqual(const std::string& lhs, const std::string& rhs) {
  if (lhs.size() != rhs.size()) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < lhs.size(); ++i) {
    difference |= static_cast<uint8_t>(lhs[i] ^ rhs[i]);
  }
  return difference == 0;
}

/** Everything one request needs to own and tear down, in the order mbedTLS wants. */
struct TlsSession {
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_net_context net;
  bool handshaken = false;

  TlsSession() {
    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
  }

  ~TlsSession() {
    if (handshaken) mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&net);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
  }
};

}  // namespace

#endif  // !SIMULATOR

AgentIslandClient::Status AgentIslandClient::connectWifi() {
#ifdef SIMULATOR
  return Status::Unsupported;
#else
  if (WiFi.status() == WL_CONNECTED) return Status::Ok;

  WIFI_STORE.loadFromFile();
  const WifiCredential* credential = WIFI_STORE.getLastCredential();
  if (!credential) return Status::NoWifi;

  WiFi.mode(WIFI_STA);
  WiFi.begin(credential->ssid.c_str(), credential->password.c_str());

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(true);
    return Status::NoWifi;
  }
  return Status::Ok;
#endif
}

void AgentIslandClient::releaseWifi() {
#ifndef SIMULATOR
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
#endif
}

#ifdef SIMULATOR

AgentIslandClient::Discovery AgentIslandClient::resolve(const AgentIslandPairing&,
                                                        const std::function<void(int, int)>&) {
  return Discovery::Failed;
}

AgentIslandClient::Result AgentIslandClient::request(const std::string&, uint16_t, const std::string&, const char*,
                                                     const char*, const std::string&, const std::string&) {
  Result result;
  result.status = Status::Unsupported;
  result.message = "Agent Island needs the radio, which the simulator does not have.";
  return result;
}

bool AgentIslandClient::probe(const std::string&, const AgentIslandPairing&) { return false; }

#else

bool AgentIslandClient::probe(const std::string& host, const AgentIslandPairing& pairing) {
  const Result result = request(host, pairing.port, pairing.fingerprint, "GET", "/health", "", "");
  return result.ok() && result.body.find("AgentIsland") != std::string::npos;
}

AgentIslandClient::Discovery AgentIslandClient::resolve(const AgentIslandPairing& pairing,
                                                        const std::function<void(int, int)>& progress) {
  address_.clear();

  // 1. Whatever answered last time. Costs one connect when the lease has not moved.
  if (!pairing.resolvedAddress.empty() && probe(pairing.resolvedAddress, pairing)) {
    address_ = pairing.resolvedAddress;
    return Discovery::Cached;
  }

  // 2. The paired hostname. `.local` needs mDNS; anything else is ordinary DNS,
  //    which connectWithTimeout() already does through getaddrinfo.
  std::string byName = pairing.host;
  const size_t dotLocal = byName.size() >= 6 ? byName.rfind(".local") : std::string::npos;
  if (dotLocal != std::string::npos && dotLocal == byName.size() - 6) {
    MDNS.begin("einx");
    const IPAddress found = MDNS.queryHost(byName.substr(0, dotLocal).c_str(), 3000);
    if (static_cast<uint32_t>(found) != 0) {
      byName = found.toString().c_str();
    }
  }
  if (probe(byName, pairing)) {
    address_ = byName;
    return Discovery::Hostname;
  }

  // 3. The sweep. DHCP moved the Mac and the name did not resolve, so walk the
  //    local /24 looking for something on 47124 and TLS-probe only what answers.
  //    The pin is what makes this safe: a stranger's listener fails the check.
  const IPAddress self = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  if (static_cast<uint32_t>(self) == 0 || mask[0] != 255 || mask[1] != 255 || mask[2] != 255) {
    return Discovery::Failed;
  }

  char candidate[16];
  for (int host = 1; host <= 254; ++host) {
    if (host == self[3]) continue;
    if (progress) progress(host, 254);
    snprintf(candidate, sizeof(candidate), "%u.%u.%u.%d", static_cast<unsigned>(self[0]), static_cast<unsigned>(self[1]),
             static_cast<unsigned>(self[2]), host);

    const int sock = connectWithTimeout(candidate, pairing.port, SCAN_CONNECT_TIMEOUT_MS);
    if (sock < 0) continue;
    close(sock);

    if (probe(candidate, pairing)) {
      address_ = candidate;
      return Discovery::Scan;
    }
  }

  return Discovery::Failed;
}

AgentIslandClient::Result AgentIslandClient::request(const std::string& host, const uint16_t port,
                                                     const std::string& fingerprint, const char* method,
                                                     const char* path, const std::string& bearer,
                                                     const std::string& body) {
  Result result;
  const unsigned long deadline = millis() + REQUEST_DEADLINE_MS;

  TlsSession session;
  const char* personalisation = "einx-agentisland";
  if (mbedtls_ctr_drbg_seed(&session.drbg, mbedtls_entropy_func, &session.entropy,
                            reinterpret_cast<const unsigned char*>(personalisation), strlen(personalisation)) != 0) {
    result.status = Status::ServerError;
    result.message = "Could not seed the random number generator.";
    return result;
  }

  const int sock = connectWithTimeout(host, port, TLS_CONNECT_TIMEOUT_MS);
  if (sock < 0) {
    result.status = Status::Unreachable;
    result.message = "No answer from " + host + ".";
    return result;
  }
  session.net.fd = sock;

  if (mbedtls_ssl_config_defaults(&session.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    result.status = Status::ServerError;
    result.message = "Could not configure TLS.";
    return result;
  }
  // NONE, not OPTIONAL: there is no CA to check against and never will be. The
  // certificate is authenticated below, by its SHA-256, exactly as the phone
  // app authenticates it — and before anything secret is written to the socket.
  mbedtls_ssl_conf_authmode(&session.conf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng(&session.conf, mbedtls_ctr_drbg_random, &session.drbg);
  mbedtls_ssl_conf_min_version(&session.conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);

  if (mbedtls_ssl_setup(&session.ssl, &session.conf) != 0) {
    result.status = Status::ServerError;
    result.message = "Could not set up the TLS session.";
    return result;
  }
  mbedtls_ssl_set_hostname(&session.ssl, host.c_str());
  mbedtls_ssl_set_bio(&session.ssl, &session.net, mbedtls_net_send, mbedtls_net_recv, nullptr);

  int handshake = 0;
  while ((handshake = mbedtls_ssl_handshake(&session.ssl)) != 0) {
    if (handshake != MBEDTLS_ERR_SSL_WANT_READ && handshake != MBEDTLS_ERR_SSL_WANT_WRITE) {
      result.status = Status::Unreachable;
      result.message = "TLS handshake failed.";
      return result;
    }
    if (static_cast<long>(millis() - deadline) >= 0) {
      result.status = Status::Unreachable;
      result.message = "TLS handshake timed out.";
      return result;
    }
  }
  session.handshaken = true;

  const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&session.ssl);
  if (peer == nullptr) {
    result.status = Status::PinMismatch;
    result.message = "The server presented no certificate.";
    return result;
  }

  uint8_t digest[32] = {0};
  if (mbedtls_sha256_ret(peer->raw.p, peer->raw.len, digest, 0) != 0) {
    result.status = Status::PinMismatch;
    result.message = "Could not fingerprint the certificate.";
    return result;
  }
  if (!digestsEqual(hexDigest(digest), fingerprint)) {
    result.status = Status::PinMismatch;
    result.message = "Certificate does not match the paired Mac.";
    Serial.printf("[%lu] [AIS] Pin mismatch at %s\n", millis(), host.c_str());
    return result;
  }

  // Verified. Only now does the token go out.
  std::string requestText;
  requestText.reserve(256 + body.size());
  requestText += method;
  requestText += " ";
  requestText += path;
  requestText += " HTTP/1.1\r\nHost: ";
  requestText += host;
  requestText += "\r\nConnection: close\r\n";
  if (!bearer.empty()) {
    requestText += "Authorization: Bearer " + bearer + "\r\n";
  }
  if (!body.empty()) {
    requestText += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  }
  requestText += "\r\n";
  requestText += body;

  size_t written = 0;
  while (written < requestText.size()) {
    if (static_cast<long>(millis() - deadline) >= 0) {
      result.status = Status::Unreachable;
      result.message = "Timed out sending the request.";
      return result;
    }
    const int sent = mbedtls_ssl_write(&session.ssl, reinterpret_cast<const unsigned char*>(requestText.data()) + written,
                                       requestText.size() - written);
    if (sent == MBEDTLS_ERR_SSL_WANT_READ || sent == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (sent <= 0) {
      result.status = Status::Unreachable;
      result.message = "Connection dropped while sending.";
      return result;
    }
    written += static_cast<size_t>(sent);
  }

  std::string response;
  unsigned char buffer[1024];
  while (response.size() < MAX_RESPONSE_BYTES) {
    if (static_cast<long>(millis() - deadline) >= 0) {
      // Whatever arrived before the deadline is still worth parsing; only an
      // empty response is a failure.
      if (response.empty()) {
        result.status = Status::Unreachable;
        result.message = "Timed out waiting for a reply.";
        return result;
      }
      break;
    }
    const int read = mbedtls_ssl_read(&session.ssl, buffer, sizeof(buffer));
    if (read == MBEDTLS_ERR_SSL_WANT_READ || read == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (read == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || read == 0) break;
    if (read < 0) {
      // A reset after the body has arrived is how "Connection: close" often
      // looks from here; only an empty response is a real failure.
      if (response.empty()) {
        result.status = Status::Unreachable;
        result.message = "Connection dropped while reading.";
        return result;
      }
      break;
    }
    response.append(reinterpret_cast<char*>(buffer), static_cast<size_t>(read));
  }

  const size_t statusEnd = response.find("\r\n");
  if (statusEnd == std::string::npos || response.compare(0, 5, "HTTP/") != 0) {
    result.status = Status::BadResponse;
    result.message = "Unrecognised reply from the Mac.";
    return result;
  }
  result.httpStatus = atoi(response.c_str() + 9);

  const size_t bodyStart = response.find("\r\n\r\n");
  if (bodyStart != std::string::npos) {
    result.body = response.substr(bodyStart + 4);
  }

  if (result.httpStatus == 401) {
    result.status = Status::Unauthorized;
    result.message = "This device is not paired, or the Mac revoked it.";
  } else if (result.httpStatus < 200 || result.httpStatus >= 300) {
    result.status = Status::ServerError;
    result.message = "Agent Island answered " + std::to_string(result.httpStatus) + ".";
  }
  return result;
}

#endif  // SIMULATOR

AgentIslandClient::Result AgentIslandClient::enroll(const AgentIslandPairing& pairing) {
  const std::string deviceId = pairing.deviceId.empty() ? AgentIslandStore::localDeviceId() : pairing.deviceId;
  const std::string body = "{\"deviceId\":\"" + deviceId + "\",\"deviceName\":\"E-inx reader\"}";
  const std::string host = address_.empty() ? pairing.host : address_;
  return request(host, pairing.port, pairing.fingerprint, "POST", "/api/pair", pairing.enrollmentToken, body);
}

AgentIslandClient::Result AgentIslandClient::fetchState(const AgentIslandPairing& pairing) {
  const std::string host = address_.empty() ? pairing.host : address_;
  return request(host, pairing.port, pairing.fingerprint, "GET", "/api/state", pairing.deviceToken, "");
}

AgentIslandClient::Result AgentIslandClient::sendCommand(const AgentIslandPairing& pairing, const std::string& json) {
  const std::string host = address_.empty() ? pairing.host : address_;
  return request(host, pairing.port, pairing.fingerprint, "POST", "/api/command", pairing.deviceToken, json);
}
