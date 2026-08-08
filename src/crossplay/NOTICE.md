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

- `solitaire/SolitaireCore.{h,cpp}` — the rules engine.
- `solitaire/SolitaireScreens.{h,cpp}` — the screen builders. They draw through a
  `DrawTarget` they are handed and know nothing about who implements it.
- `solitaire/SolitaireSuits.h` — generated 1-bpp pip artwork.
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
- `solitaire/SolitaireActivity.{h,cpp}` is a port. The rules, the save format and
  the screen models are upstream's; storage, the exit path and all input handling
  are E-inx's.
- `CrossPlayActivity.h` replaces upstream's `Shelf.cpp`, which is built on an
  activity stack E-inx does not have.
