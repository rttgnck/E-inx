# A compact `/api/state` for the reader

This describes a change to **Agent Island on the Mac**, not to E-inx. It is additive: the Mac app and the
Android companion keep the response they have today, and nothing that exists now changes shape.

## Why

`GET /api/state` returns every session's whole activity feed. That is the right answer for a phone and the
wrong one for an e-reader, which parses the reply and throws all of it away.

Measured on the machine this was built against, on 9 August 2026:

| | |
| --- | --- |
| Thirty newest Claude transcripts on disk | 197 MB |
| Activities the scanner keeps per session | 80 |
| Cap per activity | 6,000 characters |
| Resulting `/api/state` body | **~5.3 MB** |
| What the reader keeps from it | **under 4 KB** |

At what an ESP32-C3 sustains through TLS, 5.3 MB is fifteen to thirty seconds of radio per poll, for a list of
half a dozen rows. The reader already mitigates this — it streams and discards rather than buffering, and it
paces polling off how long the last fetch took instead of a fixed timer — but mitigation is all it is. The
device is downloading megabytes to render kilobytes, and no amount of care on this end changes that.

## The change

Accept a `compact` query parameter on `GET /api/state`:

```
GET /api/state?compact=1
```

When present and truthy, return the same JSON document with the fields below and **nothing else**. When absent,
return exactly what is returned today. An unknown query parameter is ignored by everything already deployed, so
shipping the Mac side first is safe.

Authentication, the pinned certificate, the port and every other route are untouched.

## What the reader reads

This is the complete set. It comes from the ArduinoJson filter in
[`AgentIslandModel.cpp`](../src/agentisland/AgentIslandModel.cpp) — the reader discards anything not listed here
before it ever reaches the panel, so sending more only costs radio time.

```jsonc
{
  "sessions": [
    {
      "id":                 "string",   // required — identifies the session in /api/command
      "provider":           "string",   // e.g. "claude"
      "title":              "string",
      "project":            "string",
      "status":             "string",   // "working" | "failed" | …
      "detail":             "string",   // one line under the title
      "lastMessage":        "string",   // the agent's last message
      "pendingActionCount": 0,          // number, drives the "N approvals waiting" row

      "pendingAction": {                // omit entirely when nothing is pending
        "id":             "string",
        "kind":           "string",
        "title":          "string",
        "summary":        "string",
        "detail":         "string",
        "canAlwaysAllow": true
      },

      "pendingPlan": {                  // omit entirely when no plan is waiting
        "id":       "string",
        "title":    "string",
        "markdown": "string"
      },

      "question": {                     // omit entirely when nothing is being asked
        "prompt":      "string",
        "header":      "string",
        "multiSelect": false,
        "isSecret":    false,
        "options": [
          { "label": "string", "description": "string" }
        ]
      }
    }
  ]
}
```

Everything else in the current response — `activities` above all, plus `cwd`, permission records, timestamps,
usage windows, and the per-session ids the panel never draws — should be omitted under `compact`.

`activities` alone is essentially the entire 5.3 MB.

## Suggested clipping

Omitting `activities` does nearly all the work. Two further limits keep a pathological case bounded, and both
are already knobs in the scanner:

- **Sessions:** cap at 30. The reader shows a scrolling list and nobody scrolls past that on a 6" panel.
- **Free text:** clip `lastMessage`, `detail` and `summary` to ~500 characters. They are drawn into a fixed box
  and wrapped to at most three lines; the rest is never seen.
- **`pendingAction.detail` and `pendingPlan.markdown`** are the two the user actually reads in full. Leave them
  at the existing 6,000-character cap.

Expect a few kilobytes in the normal case, and low tens of kilobytes with thirty busy sessions — against 5.3 MB
today.

## Implementation sketch

Illustrative only — this is written from the wire contract, not from the Agent Island source.

```swift
// In the /api/state handler
let compact = request.query["compact"].map { $0 == "1" || $0 == "true" } ?? false

if compact {
    let sessions = scanner.sessions.prefix(30).map { session in
        CompactSession(
            id:                 session.id,
            provider:           session.provider,
            title:              session.title,
            project:            session.project,
            status:             session.status,
            detail:             String(session.detail.prefix(500)),
            lastMessage:        String(session.lastMessage.prefix(500)),
            pendingActionCount: session.pendingActionCount,
            pendingAction:      session.pendingAction.map(CompactAction.init),
            pendingPlan:        session.pendingPlan.map(CompactPlan.init),
            question:           session.question.map(CompactQuestion.init)
        )
        // note: no `activities`
    }
    return try JSONEncoder().encode(CompactState(sessions: Array(sessions)))
}

return try JSONEncoder().encode(currentStatePayload())   // unchanged
```

Use `nil` for the three optional objects rather than empty ones — the reader treats a present-but-empty
`pendingAction` as an approval with no id and hides it, which is correct but wasteful.

## Verifying it

From the Mac, against its own listener:

```bash
curl -sk "https://localhost:47124/api/state" -H "Authorization: Bearer $TOKEN" | wc -c
```

```bash
curl -sk "https://localhost:47124/api/state?compact=1" -H "Authorization: Bearer $TOKEN" | wc -c
```

The second should be three orders of magnitude smaller. Then check nothing the reader needs went missing:

```bash
curl -sk "https://localhost:47124/api/state?compact=1" -H "Authorization: Bearer $TOKEN" | jq '.sessions[0] | keys'
```

And confirm the uncompacted response is byte-identical to what shipped before the change — that is the one
regression that would affect the phone.

## The reader side

**Not yet implemented.** E-inx currently requests `/api/state` with no query string, so it will keep receiving
the full payload until its request is changed to `/api/state?compact=1`.

That is a one-line change in `AgentIslandClient::fetchState`. It is deliberately not made yet, because sending a
parameter no deployed listener understands is only safe if the listener ignores unknown query parameters — worth
confirming against the real server before the reader depends on it. Once the Mac side is in, this side is
trivial.

Until then the reader copes: it streams the body instead of buffering it, paces polling off how long the last
fetch took, and shows the snapshot size in the status bar once it passes a megabyte.
