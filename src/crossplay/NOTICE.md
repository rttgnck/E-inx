# CrossPlay port — provenance and attribution

Everything under `src/crossplay/`, plus `lib/FreeInkUI/` and `lib/Icons/`, is
derived from third-party MIT-licensed work. This file records what came from
where, so a future update can be told apart from a local change.

## Upstream sources

| Source | License | Taken at |
| --- | --- | --- |
| [CrossPlay](https://github.com/ma-r-s/crossplay) (branch `xteink`) | MIT — © 2025 Dave Allie and the CrossPoint contributors; © 2026 Mario Ruiz | `195d118` |
| [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) (`libs/ui/FreeInkUI`, `libs/assets/Icons`) | MIT — © 2026 FreeInk | `2fea991` |

CrossPlay is itself a fork of [CrossPoint](https://crosspointreader.com/), which
is also what E-inx descends from by way of [Inx](https://github.com/obijuankenobiii/inx).
That shared ancestry is why this port is as small as it is.

## What was taken unchanged

Copied byte-for-byte, because they touch no firmware surface at all:

- Every `*Core.{h,cpp}` — the rules engines for all eleven apps, plus
  `chess/ChessEngine.{h,cpp}`.
- Every `*Screens.{h,cpp}` — the screen builders. They draw through a
  `DrawTarget` they are handed and know nothing about who implements it. The two
  that reference `linkui` gained one include; nothing else changed.
- `solitaire/SolitaireSuits.h`, `chess/ChessPieces.h`, `dungeon/DungeonArt.h`,
  `dungeon/DungeonPuzzles.h` — generated artwork and puzzle data.
- `chess/ChessWire.h` — the multiplayer wire format, kept so the state that would
  travel stays defined next to the game that owns it.
- `ui/ToyboxTokens.h`, `ui/ToyboxMetrics.h` — the theme as plain constants.
- `lib/FreeInkUI/`, `lib/Icons/` — the SDK, minus `FreeInkUIGfxRenderer.h`
  (see below).

## What was rewritten, and why

- `lib/FreeInkUI/include/FreeInkUIGfxRenderer.h` was **deleted** on vendoring. It
  binds FreeInkUI to CrossPoint's flat `GfxRenderer` (`renderer.fillRect`), and
  E-inx's is split into sub-renderers (`renderer.rectangle.fill`). Its
  replacement is `compat/EInxDrawTarget.{h,cpp}`.
- `compat/CrossPlayCompat.h` adapts CrossPoint's async render-task activity model
  (`render(RenderLock&&)`, `requestUpdate()`, an activity stack) to E-inx's
  single inline `loop()`. Adopting the render task firmware-wide is a separate
  decision that has been deliberately deferred; when it is taken, this file is
  deleted rather than unpicked.
- `compat/CrossPlayFocus.h` has no upstream counterpart. CrossPlay's apps are
  touch-only and the X3 has no touchscreen; this drives FreeInkUI's existing
  focus ring from the buttons.
- `ui/ToyboxFonts.h` binds the three font slots to E-inx built-ins instead of
  registering CrossPlay's twelve generated faces, whose binary format E-inx's
  `EpdFont` cannot read. See the header for what that costs.
- `ui/Toybox.h`, `ui/ToyboxTheme.h`, `ui/ToyboxScreen.h` are ports: the shapes
  and reasoning are upstream's, the renderer calls underneath are not.
- `compat/SoloLink.h` + `.cpp` stands in for upstream's whole `link/` directory
  (ESP-NOW PLAY NEARBY) and the `player/` device identity it carries. Chess and
  Battleship inherit `linkplay::LinkActivity`, so the interface is reproduced
  exactly — twelve hooks, same names, same contract — with the radio removed and
  the phase pinned to `Off`. Replacing this file is how multiplayer arrives; the
  games do not change again.
- `compat/NearbyMark.h` supplies the two `linkui` symbols those games' menus
  reference while drawing the PLAY NEARBY row.
- `compat/CrossPlayGfx.h` gives CrossPoint's flat renderer calls
  (`renderer.fillRect`) on top of E-inx's sub-renderers
  (`renderer.rectangle.fill`), so the apps' own board-drawing code is unedited.
- `compat/CrossPlayServices.h`, `compat/CrossPlayRect.h` alias storage, logging,
  the hint bar and the plain integer `Rect`.
- Each `*Activity.{h,cpp}` is a port. The rules, the save formats and the screen
  models are upstream's; storage, the exit path and all input handling are
  E-inx's. Chess is the least changed — it is the one CrossPlay app that already
  had button navigation for its board — and needed only a route to its own
  chrome, which upstream reaches by tap.
- `CrossPlayActivity.h` replaces upstream's `Shelf.cpp`, which is built on an
  activity stack E-inx does not have.
- `compat/CrossPlayWifi.h` joins CrossPoint's `startActivityForResult` push/pop to
  E-inx's `WifiSelectionActivity`, which reports through a completion callback
  instead of popping itself.
- `compat/CrossPlayTheme.h` supplies the two pieces of CrossPoint's reader theme
  Study borrows (header metrics, centred wrapped text).
- `compat/CrossPlayFontIds.h` maps upstream's generated font-id hashes onto
  E-inx's built-ins.
- `study/StudyFonts.{h,cpp}` is a **stub**. Upstream loads five subset CJK faces
  from `.cpfont` files through `SdCardFontManager`; both the format and the
  subsystem are CrossPoint's. It reports "nothing loaded", which is a state
  upstream already handles — at the cost of hanzi rendering as tofu. See the
  header.
- `crossplay::silentRestart()` tears the radio down instead of rebooting.
  Upstream's reboot depends on an RTC flag that routes you back to where you
  were; E-inx has no such flag, so a reboot would drop the player out of the app.
- `crossplay::fetchUrlStreaming()` and `crossplay::openFileForAppend()` supply two
  storage/network shapes E-inx's own APIs do not have.
