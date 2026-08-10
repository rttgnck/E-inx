#pragma once

/**
 * @file AgentIslandClient.h
 * @brief Public interface and types for AgentIslandClient.
 *
 * Agent Island's companion listener speaks HTTPS on TCP 47124 behind a
 * self-signed certificate, and the phone app pins that certificate by the
 * SHA-256 of its DER bytes — the `pin=` in the pairing payload. There is no CA
 * anywhere in the picture, which is why this does not go through
 * HttpDownloader: esp_http_client verifies against a trust anchor, and a
 * fingerprint is not one. So the transport is mbedTLS directly, with the
 * handshake left unverified by the library and the pin checked by hand
 * afterwards, before a single byte of the request — bearer token included —
 * goes out.
 *
 * Everything here blocks. It is called from the activity's loop between
 * repaints, never from a render path.
 */

#include <cstdint>
#include <functional>
#include <string>

#include "network/ByteReader.h"
#include "state/AgentIslandPairing.h"

class AgentIslandClient {
 public:
  enum class Status : uint8_t {
    Ok,
    NoPairing,       ///< Nothing has been pasted into the web manager yet.
    NoWifi,          ///< No saved network, or the radio would not associate.
    Unreachable,     ///< Nothing answered on 47124 at any address we could find.
    PinMismatch,     ///< Something answered, and it was not the Mac we paired with.
    Unauthorized,    ///< The Mac has revoked this device, or the pairing code expired.
    ServerError,     ///< The Mac answered with a 4xx/5xx we did not ask for.
    BadResponse,     ///< Answered, but not with the JSON we expect.
    Unsupported,     ///< Simulator build: there is no radio here.
    Cancelled,       ///< The user pressed Back while this was in flight.
    NotModified,     ///< 304: the revision we hold is still current. Not an error.
  };

  struct Result {
    Status status = Status::Ok;
    int httpStatus = 0;
    std::string body;         ///< Buffered response body; empty when one was streamed instead.
    std::string message;      ///< Human-readable failure, safe to put on the panel.
    size_t bytesReceived = 0; ///< Body bytes off the wire, however they were consumed.
    uint32_t elapsedMs = 0;   ///< How long the whole exchange took, for pacing the next one.

    bool ok() const { return status == Status::Ok; }
  };

  /**
   * Consumes a 2xx body as it arrives, for responses too big to hold. Returning
   * false marks the request as BadResponse; `message` should say why.
   */
  using BodyHandler = std::function<bool(inx::ByteReader& reader, std::string& message)>;

  /** How the endpoint was found, so the UI can say "found it at 192.168.1.42" once. */
  enum class Discovery : uint8_t { Cached, Hostname, Scan, Failed, Cancelled };

  explicit AgentIslandClient() = default;

  /**
   * Polled from inside every blocking loop — the TLS handshake, the body read,
   * and between the resolver's strategies. Return true to give up.
   *
   * Everything this class does blocks the calling thread, and some of it blocks
   * for tens of seconds: a Mac that is switched off takes the resolver through
   * mDNS and 253 connects, and a busy one answers /api/state with megabytes.
   * The main loop is not running to poll GPIO while any of that happens, so
   * without a hook the panel is frozen and the reset pin is the only way out.
   */
  void setAbortCheck(std::function<bool()> abort) { abort_ = std::move(abort); }

  /** Brings up the radio from the saved credential. Idempotent once associated. */
  Status connectWifi();
  /** Drops the radio. Called on the way out of the activity, as E-inx's other networked screens do. */
  static void releaseWifi();

  /**
   * Finds the Mac: the address that answered last time, then the paired
   * hostname over mDNS/DNS, then a sweep of the local /24 for a /health that
   * presents the pinned certificate. The winner is remembered for next time.
   *
   * The sweep is 253 connects. The sweep is 253 connects and the whole thing runs on the
   * calling thread, so `progress` is also the abort hook — return false from it
   * and resolve() stops and reports Cancelled. Without that a reader whose Mac
   * is switched off looks frozen for the best part of a minute with no way out
   * but the reset pin.
   */
  Discovery resolve(const AgentIslandPairing& pairing, const std::function<bool(int scanned, int total)>& progress);

  /** POST /api/pair with the enrollment secret. On success the device token is stored. */
  Result enroll(const AgentIslandPairing& pairing);
  /**
   * GET /api/state, handing the body to `onBody` as it arrives rather than
   * buffering it. The snapshot carries every session's whole activity feed and
   * has no useful upper bound, so buffering it is not an option on this device.
   */
  /**
   * `knownRev` is the revision of the snapshot already held, sent as
   * If-None-Match. When the Mac's model has not moved it answers 304 with no
   * body and the result is Status::NotModified — a round trip instead of a
   * transfer, which on this link is the difference between a second and
   * minutes. Pass an empty string to force a full fetch.
   */
  Result fetchState(const AgentIslandPairing& pairing, const std::string& knownRev, const BodyHandler& onBody);

  /**
   * One session's approval detail, plan body and question options. The compact
   * list carries ids and titles only, so this is what a card is drawn from and
   * it is fetched when the card opens, never for the list.
   */
  Result fetchSessionDetail(const AgentIslandPairing& pairing, const std::string& sessionId,
                            const BodyHandler& onBody);
  /** POST /api/command with an already-serialised JSON body. */
  Result sendCommand(const AgentIslandPairing& pairing, const std::string& json);

  const std::string& address() const { return address_; }

 private:
  /**
   * One request, one connection. Agent Island answers `Connection: close` on
   * every route, so there is no keep-alive to be had and nothing to reuse.
   *
   * `bearer` is empty for the health probe, the enrollment secret for /api/pair,
   * and the device token otherwise. When `onBody` is set and the status is 2xx,
   * the body is streamed to it and `result.body` is left empty; otherwise it is
   * buffered, which every route but /api/state can afford. `budgetMs` bounds the
   * whole exchange — short for a probe during the subnet sweep, generous for a
   * snapshot that may run to hundreds of kilobytes.
   */
  Result request(const std::string& host, uint16_t port, const std::string& fingerprint, const char* method,
                 const std::string& path, const std::string& bearer, const std::string& body, uint32_t budgetMs,
                 const BodyHandler* onBody = nullptr, const std::string& extraHeaders = std::string());

  /** GET /health against `host`, used by both the resolver and the scan. */
  bool probe(const std::string& host, const AgentIslandPairing& pairing);

  /** True when the caller wants out; never null-checked at the call sites, so guard it. */
  bool aborted() const { return abort_ && abort_(); }

  std::function<bool()> abort_;
  std::string address_;
};
