# E-inx AgentIsland — making the reader's side work

For whoever is working on the **E-inx firmware**. The companion document,
[`agentisland-mac-plan.md`](agentisland-mac-plan.md), covers the Mac. The Mac's changes should land first; this
side is small once they have.

## The situation, measured

On 9 August 2026, one `/api/state` fetch, instrumented on the device:

```
[AIS] Body 442273 B in 180933 ms: ssl_read 174129 ms over 624 fills, abort hook 21 ms
```

The same request, same moment, same network:

| Client | Time |
| --- | --- |
| `curl` on the Mac (loopback) | 0.00 s |
| `curl` from another LAN machine | **0.088 s** |
| This reader | **180.9 s** |

**The Mac is not slow. Our link is.** 442 KB at roughly **2.4 KB/s**, with 96% of the wall time parked inside
`mbedtls_ssl_read` waiting for bytes that have not arrived. The device is not parsing slowly and the abort hook
costs 21 ms across three minutes — neither is worth optimising.

Two orders of magnitude of that gap is a transport problem nobody has diagnosed yet (see "The throughput
question" below). **Do not start there.** The payload work below makes the app usable regardless of whether the
transport is ever fixed, and it is the part we control.

## What is already done

Do not redo these:

- `/api/state` is **streamed and parsed as it arrives**, never buffered — the body has no useful upper bound.
- Polling **paces off how long the last fetch took** (`AgentIslandActivity::pollInterval()`), floored at
  `POLL_INTERVAL_MS` (8 s) and capped at `POLL_INTERVAL_MAX_MS` (120 s).
- **Back interrupts any blocking network call.** `AgentIslandClient::setAbortCheck()` is polled from the TLS
  handshake, the body reader, and the resolver; the activity's hook is level-triggered (`isPressed`, not
  `wasPressed`) because the poll rate is too coarse to catch an edge reliably.
- The socket's `SO_RCVTIMEO` is 250 ms, which is what gives the read loop enough opportunities to notice Back.
  **`tv_usec` must carry the sub-second remainder** — leaving it zero makes the timeout zero, which lwIP reads
  as "block forever". That bug has been fixed once already; do not reintroduce it.

## The four changes

### 1. Request the compact projection

`AgentIslandClient::fetchState()` currently requests `/api/state` bare
([`AgentIslandClient.cpp:718`](../src/network/AgentIslandClient.cpp)). Change the path to
`/api/state?compact=1`.

This is one line and it is the reason none of the Mac-side work has taken effect so far — the compact endpoint
has never been requested.

### 2. Send `If-None-Match`, and handle 304

The largest win on this side. Most polls change nothing, and a 304 costs a round trip instead of a transfer.

- Keep the `rev` from the last successful snapshot on the activity.
- Send it as `If-None-Match` on each `/api/state` request. This needs a request-header argument threaded
  through `AgentIslandClient::request()`, which currently builds its headers internally.
- On **304**: do not parse, do not touch the snapshot, do not repaint. Treat it exactly as an unchanged
  signature is treated today — the panel already refuses to redraw when nothing moved, and this is the same
  idea one layer earlier.

Note the existing status handling maps non-2xx to `ServerError`/`BadResponse`; 304 must be threaded through as
its own outcome rather than an error.

### 3. Fetch card detail on demand

The list no longer carries `pendingAction.detail`, `pendingPlan.markdown`, or question options — the Mac sends
only ids and titles. The panel needs those bodies only when a card opens.

- In `AgentIslandActivity::openSelected()`, before switching to `Phase::Card`, request
  `GET /api/session/{id}` and merge the result into the open session.
- Do it through the existing `request(Work::…)` mechanism so the busy screen is drawn *before* the blocking
  call — the same rule the rest of this activity follows, and the reason a fetch never happens on the render
  path.
- A failure here should show a notice and return to the list, not leave a card half-populated.

### 4. Re-tune the polling

Once a poll is a 304 or ~12 KB, the 120 s backoff is doing harm rather than good: an approval can sit unseen
for two minutes, which is the one thing this app exists to prevent.

- Drop `POLL_INTERVAL_MAX_MS` to something like 30 s.
- The `lastFetchMs_ * 3` pacing can stay — it will simply stop being the binding constraint.
- Reconsider the `LARGE_SNAPSHOT_BYTES` status-bar note (1 MB). After this work a snapshot that large means
  something is wrong; consider lowering it to ~64 KB so it stays a useful signal.

## The throughput question — separate, and after

2.4 KB/s against a server that answers in 88 ms is pathological. A C3 should manage hundreds of KB/s through
TLS. Worth investigating **once the payload work is in**, because it stops being urgent then.

The instrumentation already in `TlsBodyReader` is the tool: 624 fills for 442,273 bytes is ~709 bytes per read
at ~290 ms each. Data arriving roughly one TCP segment at a time, a quarter-second apart, is the signature of a
window or ACK-pacing problem rather than raw slowness.

Worth trying, cheapest first:

- `TCP_NODELAY` on the socket in `connectWithTimeout()`.
- lwIP's receive window (`CONFIG_LWIP_TCP_WND_DEFAULT`). Note this is baked into Arduino-ESP32's precompiled
  lwIP, so it may need an ESP-IDF component build to change — check before committing to it.
- Whether mbedTLS is negotiating a small record/fragment size.

Measure with a fixed-size fetch against the Mac, comparing bytes-per-fill and ms-per-fill before and after each
change. Do not change two things at once.

## Acceptance

Flash, plug in USB, and watch the serial log — `115200`, and **DTR and RTS must both be deasserted**, or the
C3's native USB holds the chip in reset and you will capture nothing.

Then:

- Opening the app logs `[AIS] Body …` with a body of ~12 KB, not hundreds of KB.
- A poll with nothing changed logs no body at all and does not repaint.
- Opening a card fetches its own detail and draws it.
- Back during any fetch logs `Back pressed during a network call; leaving` and exits promptly.
- `ssl_read` time falls roughly in proportion to the byte count. If it does not, that is the transport problem
  above, not this work.

## Expected result

| | Now | After |
| --- | --- | --- |
| Poll, nothing changed | 442 KB / 181 s | ~0.2 KB / **under 1 s** |
| Poll, something changed | 442 KB / 181 s | ~12 KB / **~5 s** |
| Opening a card | already in payload | ~6 KB / **~2.5 s** |
| Poll interval | 120 s | ~10 s |
