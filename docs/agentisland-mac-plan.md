# Agent Island (Mac) — what the E-inx reader needs from you

For whoever is working on the **Mac** app. There is a companion document,
[`agentisland-einx-plan.md`](agentisland-einx-plan.md), for the reader side. The two changes are designed to
land independently, and the Mac's can go first.

This supersedes `agentisland-compact-state.md`, which recommended server-side caching. Measurement has since
shown that to be unnecessary — see below. Do not do it.

## The situation, measured

The reader is an ESP32-C3 talking TLS over Wi-Fi. On 9 August 2026, fetching `/api/state`:

| Client | Payload | Time |
| --- | --- | --- |
| `curl` on the Mac (loopback) | ~442 KB | **0.00 s** |
| `curl` from another LAN machine | ~442 KB | **0.088 s** |
| E-inx reader | 442 KB | **180.9 s** |

Of the reader's 180.9 s, **174.1 s was spent inside `mbedtls_ssl_read` waiting for bytes.** The device is idle
for 96% of that; it is not parsing slowly, it is waiting.

**Your app is not the bottleneck.** It produces and serves the whole thing in 88 ms across the network. Nothing
about generation, threading, or caching needs to change, and adding a cache would be effort spent on a
component already three orders of magnitude faster than it needs to be.

The reader's link runs at roughly **2.4 KB/s**. That is the constant to design against. Every kilobyte you do
not send is about 0.4 seconds the user does not wait.

Two further facts that shape the design:

- Observed payloads swung between 442 KB and 5.3 MB across three fetches, tracking session content.
- The reader polls repeatedly. Today **every poll transfers the entire document**, whether or not anything
  changed. At 2.4 KB/s a 442 KB poll is three minutes, so the reader has backed its interval off to 120 s
  simply to avoid overlapping fetches — which makes approvals arrive up to two minutes late, defeating the
  point of the app.

## What the reader can actually display

Twenty-five fields. This is the complete set, taken from the ArduinoJson filter in
[`AgentIslandModel.cpp`](../src/agentisland/AgentIslandModel.cpp) — anything else on the wire is parsed and
dropped.

A list row needs id, title, project, status, one line of detail, one line of last message, and a count:
**200–400 bytes**. Thirty of those is 6–12 KB. The bulky fields matter for exactly one session — the one the
user opened.

## Three changes

### 1. `rev` and `304 Not Modified` — do this first

The single biggest win, and it works even if you do nothing else.

Add a monotonic revision to the top of the response — a counter bumped whenever the session model changes, or a
hash of it:

```jsonc
{ "rev": 1741, "sessions": [ … ] }
```

Honour `If-None-Match` on `GET /api/state`. When the client's value matches the current `rev`, return **304
with an empty body**.

Most polls change nothing. Today each of those costs 442 KB and three minutes; after this they cost a couple of
hundred bytes and a round trip. This alone makes the app usable.

Use the same `rev` for compact and full responses — it describes the model, not the projection.

### 2. `?compact=1` — the list projection

```
GET /api/state?compact=1
```

Return the same document shape with these fields and **nothing else**:

```jsonc
{
  "rev": 1741,
  "sessions": [
    {
      "id":                 "string",   // required — identifies the session to /api/command
      "provider":           "string",
      "title":              "string",
      "project":            "string",
      "status":             "string",   // "working" | "failed" | …
      "detail":             "string",   // clip to ~500 chars
      "lastMessage":        "string",   // clip to ~500 chars
      "pendingActionCount": 0,

      // Presence tells the reader which badge to draw. Send the ids and titles
      // only — never the bodies. Omit the key entirely when not applicable.
      "pendingAction": { "id": "string", "kind": "string", "title": "string" },
      "pendingPlan":   { "id": "string", "title": "string" },
      "question":      { "header": "string", "multiSelect": false, "isSecret": false }
    }
  ]
}
```

Omit `activities` entirely — it is essentially the whole payload. Also omit `cwd`, permission records,
timestamps, usage windows, and any id the panel does not draw.

Cap the list at **30 sessions**. Nobody scrolls past that on a 6" panel.

**Target: under 16 KB.** At 2.4 KB/s that is a ~6 second fetch, and only when something actually changed.

### 3. `GET /api/session/{id}` — the detail projection

Fetched only when the user opens a card, so exactly one of these is ever in flight:

```jsonc
{
  "id": "string",
  "pendingAction": {
    "id": "string", "kind": "string", "title": "string",
    "summary": "string", "detail": "string", "canAlwaysAllow": true
  },
  "pendingPlan": { "id": "string", "title": "string", "markdown": "string" },
  "question": {
    "prompt": "string", "header": "string", "multiSelect": false, "isSecret": false,
    "options": [ { "label": "string", "description": "string" } ]
  }
}
```

Keep the existing 6,000-character cap on `detail` and `markdown` — these are the two fields a user genuinely
reads in full.

## Compatibility

- **No `compact` parameter means today's response, byte for byte.** The Mac and Android apps must be unaffected.
- Adding `rev` to the full response is additive; existing clients ignore unknown keys.
- A client that sends no `If-None-Match` must always get a 200 with a body.
- `/api/pair`, `/api/command`, the port, the pinned certificate and the bearer scheme are all unchanged.

## Non-goals

- **Do not add caching or a background rebuild.** 88 ms is already fast enough; this would be work with no
  measurable effect.
- Do not change the default response shape.
- Do not compress. The reader has no gzip path, and at these sizes it would not be the constraint.

## Acceptance

```bash
# Compact list — expect well under 16 KB
curl -sk "https://localhost:47124/api/state?compact=1" -H "Authorization: Bearer $TOKEN" | wc -c
```

```bash
# Unchanged state — expect 304 and an empty body
REV=$(curl -sk "https://localhost:47124/api/state?compact=1" -H "Authorization: Bearer $TOKEN" | jq -r .rev)
curl -sk -o /dev/null -w '%{http_code} %{size_download}\n' \
  "https://localhost:47124/api/state?compact=1" -H "Authorization: Bearer $TOKEN" -H "If-None-Match: $REV"
```

```bash
# The full response must be untouched — diff against a capture from before the change
curl -sk "https://localhost:47124/api/state" -H "Authorization: Bearer $TOKEN" | jq -S 'del(.rev)' > after.json
```

Then confirm the compact projection still carries everything the reader draws:

```bash
curl -sk "https://localhost:47124/api/state?compact=1" -H "Authorization: Bearer $TOKEN" | jq '.sessions[0] | keys'
```

## Expected result

| | Now | After |
| --- | --- | --- |
| Poll, nothing changed | 442 KB / 181 s | ~0.2 KB / **under 1 s** |
| Poll, something changed | 442 KB / 181 s | ~12 KB / **~5 s** |
| Opening a card | already in payload | ~6 KB / **~2.5 s** |
| Poll interval | 120 s | back to ~10 s |
