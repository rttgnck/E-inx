/**
 * @file AgentIslandClient.cpp
 * @brief Definitions for AgentIslandClient.
 */

#include "network/AgentIslandClient.h"

#include <HardwareSerial.h>

#include <algorithm>
#include <cstring>

#include "state/NetworkCredential.h"

namespace {
/**
 * Ceilings on one exchange. mbedtls_net_recv() reports an expired SO_RCVTIMEO
 * as MBEDTLS_ERR_SSL_WANT_READ, which is indistinguishable from "call me
 * again" — so a stalled peer would spin the retry loops forever without a
 * deadline of our own. The probe's is short because 253 of them make a sweep;
 * the snapshot's is long because it can run to hundreds of kilobytes.
 */
constexpr uint32_t PROBE_BUDGET_MS = 8000;
constexpr uint32_t REQUEST_BUDGET_MS = 45000;
}  // namespace

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
 * The cap on a *buffered* body — /health, /api/pair, /api/command and any error
 * page, all of which are a line or two. /api/state is streamed and is not
 * subject to this; it used to be, and truncating it mid-JSON is exactly what
 * made the panel say "unexpected reply".
 */
constexpr size_t MAX_BUFFERED_BYTES = 8 * 1024;

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

/**
 * Reads the response body off an open TLS session, starting with whatever was
 * already pulled in while scanning for the end of the headers. Honours
 * Content-Length when the server gave one and otherwise reads to close.
 */
class TlsBodyReader final : public inx::ByteReader {
 public:
  TlsBodyReader(mbedtls_ssl_context* ssl, std::string leftover, const long contentLength,
                const unsigned long deadline)
      : ssl_(ssl), buffer_(std::move(leftover)), remaining_(contentLength), deadline_(deadline) {
    if (remaining_ >= 0) {
      const long alreadyHave = static_cast<long>(buffer_.size());
      remaining_ = remaining_ > alreadyHave ? remaining_ - alreadyHave : 0;
    }
  }

  int read() override {
    char one = 0;
    return readBytes(&one, 1) == 1 ? static_cast<unsigned char>(one) : -1;
  }

  size_t readBytes(char* out, const size_t length) override {
    size_t produced = 0;
    while (produced < length) {
      if (offset_ < buffer_.size()) {
        const size_t take = std::min(length - produced, buffer_.size() - offset_);
        memcpy(out + produced, buffer_.data() + offset_, take);
        offset_ += take;
        produced += take;
        continue;
      }
      if (finished_ || !fill()) break;
    }
    return produced;
  }

  bool complete() const { return remaining_ <= 0; }

 private:
  bool fill() {
    if (remaining_ == 0) {
      finished_ = true;
      return false;
    }
    if (static_cast<long>(millis() - deadline_) >= 0) {
      finished_ = true;
      return false;
    }

    unsigned char chunk[1024];
    size_t want = sizeof(chunk);
    if (remaining_ > 0 && static_cast<size_t>(remaining_) < want) want = static_cast<size_t>(remaining_);

    const int got = mbedtls_ssl_read(ssl_, chunk, want);
    if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE) return true;
    if (got <= 0) {
      // A close with no Content-Length is the end of the body, not an error.
      if (remaining_ < 0) remaining_ = 0;
      finished_ = true;
      return false;
    }

    buffer_.assign(reinterpret_cast<char*>(chunk), static_cast<size_t>(got));
    offset_ = 0;
    if (remaining_ > 0) remaining_ -= got;
    return true;
  }

  mbedtls_ssl_context* ssl_;
  std::string buffer_;
  size_t offset_ = 0;
  long remaining_;
  unsigned long deadline_;
  bool finished_ = false;
};

/** Case-insensitive lookup of one header value in a raw header block. */
std::string headerValue(const std::string& headers, const std::string& name) {
  std::string lowered = headers;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](const unsigned char c) { return static_cast<char>(tolower(c)); });
  const size_t at = lowered.find("\r\n" + name + ":");
  if (at == std::string::npos) return "";
  const size_t valueStart = headers.find(':', at + 2) + 1;
  const size_t valueEnd = headers.find("\r\n", valueStart);
  if (valueEnd == std::string::npos) return "";
  std::string value = headers.substr(valueStart, valueEnd - valueStart);
  while (!value.empty() && isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
  while (!value.empty() && isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  return value;
}

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
                                                     const char*, const std::string&, const std::string&, uint32_t,
                                                     const BodyHandler*) {
  Result result;
  result.status = Status::Unsupported;
  result.message = "Agent Island needs the radio, which the simulator does not have.";
  return result;
}

bool AgentIslandClient::probe(const std::string&, const AgentIslandPairing&) { return false; }

#else

bool AgentIslandClient::probe(const std::string& host, const AgentIslandPairing& pairing) {
  const Result result = request(host, pairing.port, pairing.fingerprint, "GET", "/health", "", "", PROBE_BUDGET_MS);
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
                                                     const std::string& body, const uint32_t budgetMs,
                                                     const BodyHandler* onBody) {
  Result result;
  const unsigned long deadline = millis() + budgetMs;

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

  // Read only as far as the end of the headers. What follows is the body, and
  // whether it is buffered or streamed is decided from the status line.
  std::string head;
  size_t headerEnd = std::string::npos;
  unsigned char buffer[512];
  while (headerEnd == std::string::npos) {
    if (static_cast<long>(millis() - deadline) >= 0) {
      result.status = Status::Unreachable;
      result.message = "Timed out waiting for a reply.";
      return result;
    }
    const int read = mbedtls_ssl_read(&session.ssl, buffer, sizeof(buffer));
    if (read == MBEDTLS_ERR_SSL_WANT_READ || read == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (read <= 0) {
      result.status = Status::Unreachable;
      result.message = "Connection dropped before the headers arrived.";
      return result;
    }
    head.append(reinterpret_cast<char*>(buffer), static_cast<size_t>(read));
    headerEnd = head.find("\r\n\r\n");
    if (headerEnd == std::string::npos && head.size() > MAX_BUFFERED_BYTES) {
      result.status = Status::BadResponse;
      result.message = "The reply had no end to its headers.";
      return result;
    }
  }

  if (head.compare(0, 5, "HTTP/") != 0) {
    result.status = Status::BadResponse;
    result.message = "Unrecognised reply from the Mac.";
    return result;
  }
  result.httpStatus = atoi(head.c_str() + 9);

  const std::string headers = head.substr(0, headerEnd + 2);
  std::string leftover = head.substr(headerEnd + 4);
  const std::string lengthHeader = headerValue(headers, "content-length");
  const long contentLength = lengthHeader.empty() ? -1 : atol(lengthHeader.c_str());

  if (result.httpStatus == 401) {
    result.status = Status::Unauthorized;
    result.message = "This device is not paired, or the Mac revoked it.";
  } else if (result.httpStatus < 200 || result.httpStatus >= 300) {
    result.status = Status::ServerError;
    result.message = "Agent Island answered " + std::to_string(result.httpStatus) + ".";
  }

  TlsBodyReader reader(&session.ssl, std::move(leftover), contentLength, deadline);

  if (onBody != nullptr && result.ok()) {
    std::string message;
    if (!(*onBody)(reader, message)) {
      result.status = Status::BadResponse;
      result.message = message.empty() ? "The reply could not be read." : message;
    } else if (!reader.complete()) {
      // The parser stopped early — a body cut short by the deadline or a close
      // looks like valid JSON that simply ends, so say so rather than let a
      // half-read session list stand in for the truth.
      result.status = Status::BadResponse;
      result.message = "The reply arrived incomplete.";
    }
    return result;
  }

  // Buffered: every route but /api/state, plus any error body.
  char chunk[512];
  while (result.body.size() < MAX_BUFFERED_BYTES) {
    const size_t got = reader.readBytes(chunk, sizeof(chunk));
    if (got == 0) break;
    result.body.append(chunk, got);
  }
  return result;
}

#endif  // SIMULATOR

AgentIslandClient::Result AgentIslandClient::enroll(const AgentIslandPairing& pairing) {
  const std::string deviceId = pairing.deviceId.empty() ? AgentIslandStore::localDeviceId() : pairing.deviceId;
  const std::string body = "{\"deviceId\":\"" + deviceId + "\",\"deviceName\":\"E-inx reader\"}";
  const std::string host = address_.empty() ? pairing.host : address_;
  return request(host, pairing.port, pairing.fingerprint, "POST", "/api/pair", pairing.enrollmentToken, body,
                 REQUEST_BUDGET_MS);
}

AgentIslandClient::Result AgentIslandClient::fetchState(const AgentIslandPairing& pairing, const BodyHandler& onBody) {
  const std::string host = address_.empty() ? pairing.host : address_;
  return request(host, pairing.port, pairing.fingerprint, "GET", "/api/state", pairing.deviceToken, "",
                 REQUEST_BUDGET_MS, &onBody);
}

AgentIslandClient::Result AgentIslandClient::sendCommand(const AgentIslandPairing& pairing, const std::string& json) {
  const std::string host = address_.empty() ? pairing.host : address_;
  return request(host, pairing.port, pairing.fingerprint, "POST", "/api/command", pairing.deviceToken, json,
                 REQUEST_BUDGET_MS);
}
