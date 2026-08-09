# E-inx

Reimagined. Improved. Simplified.

E-inx is a community firmware for Xteink e-paper readers, forked from [Inx](https://github.com/obijuankenobiii/inx) with significant new features and improvements. It is focused on a cleaner reading experience, better EPUB support, native image rendering, SD-card fonts, and practical device tools.

*This project is a fork of Inx / CrossPoint and is not affiliated with Xteink.*

---

![](./docs/images/cover.jpg)

## What's New in E-inx 1.5.17-4_update

- **AgentIsland** — a fourth entry in the app drawer, talking to the Agent Island companion listener on your Mac.
  It lists the sessions that are actually live,
  shows the last thing each agent said, and answers the one that is blocked: tool approvals (allow, deny, always
  allow), plan reviews (accept, accept & auto-approve, reject), and multiple-choice questions including
  multi-select. Nothing on the Mac side changed to make this work.
- **It is deliberately not the phone app.** No session history, no usage windows, no settings mirror — the panel is
  a place you answer from, which is the Wear companion's job rather than the Android one's. Anything needing text
  typed — a plan revision, a free-form question, a protected answer — says so and points at the phone or the Mac
  instead of pretending.
- **A status bar of its own**, because the drawer's lives at the bottom: connection state on the left (a filled dot
  and the Mac's address when it is answering), clock and battery on the right.
- **Certificate pinning, done properly.** Agent Island's companion listener is HTTPS on 47124 behind a self-signed
  certificate that the phone app pins by SHA-256. There is no CA in that picture, so this does not go through
  `HttpDownloader` — `esp_http_client` verifies against a trust anchor and a fingerprint is not one. The transport
  is mbedTLS directly: handshake unverified by the library, fingerprint checked by hand straight afterwards, and
  not one byte of the request — bearer token included — written before it matches.
- **Pairing without a camera.** The X3 cannot scan the QR code, so the web manager takes it instead: Sync ›
  Library Server, then Settings › Agent Island, and paste the `agentisland://pair…` link the Mac shows under its
  code. The device token is exchanged on first connect and the enrollment secret is discarded.
- **Finding the Mac when DHCP moves it** — the address that answered last time, then the paired hostname over
  mDNS, then a sweep of the local /24 for a listener presenting the pinned certificate. The pin is what makes the
  sweep safe: nothing else on the network can answer for the Mac.

### Known limitations in this release

- **Not run on hardware.** Neither the pairing flow nor a live approval has been exercised on a device.
- **Foreground only.** The app polls while it is open and drops the radio when you leave it; there is no
  background wake and no notification when an approval arrives with the reader asleep.
- **Session controls are not here.** Opening a session on the Mac and dismissing one are in the phone app and not
  in this one.
- Flash 80.7% (5,285,876 of 6,553,600) — about 53KB over 1.5.17-3. mbedTLS was already linked for the HTTPS
  downloader, so the TLS is nearly free; what is new is the client, the model and the screens.

## What's New in E-inx 1.5.17-3_update

- **The rest of CrossPlay** — Jaipur, Murdle, Connections, Insider, Study, Hacker News and xkcd join the four
  already ported. The `CrossPlay` entry now lists eleven apps and scrolls.
- **App drawer rows fit their contents** — the row border was exactly as tall as the text block inside it, so
  every description had its descenders clipped by the box. Row geometry is now derived from the fonts, in the
  Apps, Games and CrossPlay lists alike, so a font change cannot silently re-break it.
- **Buttons throughout** — six of the seven new apps route everything through the interaction table, so the
  focus ring reaches them as-is. Murdle and Jaipur needed cursors of their own (a grid and a clue list; a row of
  market, hand and herd cells), and Study's four grade cells became a cursor the pad walks.
- **Networking without a reboot** — Connections, Hacker News and xkcd end a Wi-Fi session by tearing the radio
  down, the way E-inx's own networked screens do. Upstream reboots the device at that point, which here would
  drop you out of the app and back to the home screen every time you closed one.

### Known limitations in this release

- **Study will not draw Chinese.** Its five CJK faces are CrossPoint `.cpfont` files loaded through a font
  subsystem E-inx does not have. Latin-script decks, the FSRS scheduler and the whole review flow work; a deck
  whose headwords are hanzi will show boxes for them.
- **PLAY NEARBY still does nothing** — now in Jaipur as well as Chess and Battleship.
- Typography, corner radii and grey levels are unchanged from 1.5.17 — see below.
- **Only Solitaire has been run on hardware.**

## What's New in E-inx 1.5.17-2_update

- **Tier 1 of the CrossPlay port** — Chess, Battleship and D&Diagrams join Solitaire under the `CrossPlay`
  entry in the app drawer. All four are single-player.
- **Chess** — a full engine on the device, with adjustable strength, undo, a resumable save and settings.
- **Battleship** — arrange five ships, then hunt the computer's fleet, with a per-ship roster of what has gone down.
- **D&Diagrams** — a nonogram whose clues are a dungeon, 64 of them plus a tutorial and an adventurer's guide.
- **Buttons on every play surface** — the port's second input problem, and a different one from Solitaire's:
  these games draw their boards straight to the renderer, so their cells are not in the interaction table and
  the focus ring cannot reach them. The direction pad now moves a cursor on the board, Confirm acts where it
  points, and the paging pair steps through the on-screen chrome. A direction press always hands the buttons
  back to the board, so there is no mode to get stuck in.
- **PLAY NEARBY is present but does nothing yet.** Chess and Battleship are built on CrossPlay's multiplayer
  base class, so the interface is ported and the radio is not. The menu row keeps its mark and says why it
  declined rather than disappearing.

### Known limitations in this release

- Same three as 1.5.17 (typography, corner radii and grey levels, no hardware testing) — see below.
- **Chess is the slowest app here.** The engine searches on the same core that drives the panel, so a hard
  difficulty means a visible pause on the device's move. Upstream has the same shape.

## What's New in E-inx 1.5.17

- **CrossPlay in the app drawer** — a new `CrossPlay` entry sits alongside News and Games, hosting apps ported
  from [CrossPlay](https://github.com/ma-r-s/crossplay), a CrossPoint fork for the Xteink X4 Pro. This release
  ships the porting layer plus its first app.
- **Solitaire** — Klondike, in landscape, with draw-one and draw-three, undo, auto-finish, a resumable save, and
  a win/streak record kept on the SD card under `/.einx/crossplay/`.
- **Button play for a touch-only game** — CrossPlay's apps are written for the X4 Pro's touchscreen and are
  unplayable as-is on the button-only X3. Input now comes from the buttons through FreeInkUI's focus ring, which
  highlights the current control and needs no change to any screen. Confirm on a card column you are already
  holding digs one card deeper into the run, which is how multi-card moves work without a finger to point with.
- **A reusable porting layer** — `src/crossplay/compat/` binds FreeInkUI to E-inx's renderer and adapts
  CrossPoint's split `loop()`/`render()` activity model to E-inx's. The remaining CrossPlay apps sit on top of
  this rather than each needing their own adaptation.

### Known limitations in this release

- **Typography is E-inx's, not CrossPlay's.** CrossPlay's Jersey 25 and Noto Serif cuts are in CrossPoint's font
  binary format, which is not E-inx's, so the screens are set in Atkinson Hyperlegible. Header titles are 18px
  where the design wants 30px and therefore sit in more air than intended.
- **Corner radii and grey levels are approximate.** E-inx's renderer derives a rounded rect's radius from its box
  and has three fill tones to FreeInkUI's four, so light and dark grey render identically.
- **Not yet run on hardware.** This builds clean for the X3 and the CrossPlay sources it is built from have never
  been run on a physical device by anyone, upstream included.

## What's New in E-inx 1.3.17-5_update

- **Live GitHub update progress in the Web UI** — GitHub firmware installation now runs in the background while
  the page polls a dedicated status endpoint and shows percentage, transferred bytes, validation, errors, and the
  pending reboot. Reloading the page reconnects to an update already in progress.
- **Visible device download/install progress** — both on-device GitHub update paths show a combined streamed
  download-and-install bar with percentage and byte counts.
- **E-ink-friendly progress refreshes** — progress counters are thread-safe and device screens redraw at useful
  five-percent intervals, avoiding a frozen-looking bar without needlessly flashing on every network chunk.
- **Wider Update Server action** — the bottom hint automatically expands and reflows so `GitHub Update` fits inside
  its button for every supported front-button mapping.

## What's New in E-inx 1.3.17-4_update

- **GitHub Update on the Update Server screen** — the existing Update Server continues to start normally, while
  the second bottom button beside `« Back` now reads `GitHub Update`.
- **Direct device update flow** — pressing the new button uses the active Wi-Fi connection to check the latest
  E-inx release, show its version and scrollable changelog, request confirmation, download, validate, install, and
  restart the reader.
- **Safe return to the server** — cancelling, finding no newer release, or backing out after an error restores the
  existing Update Server without changing its browser-based upload options.

## What's New in E-inx 1.3.17-3_update

- **GitHub firmware updates on the reader** — the Firmware Update screen now has an explicit GitHub update action
  that checks the latest E-inx release, shows its version and changelog, and asks for confirmation before download
  and installation. Long changelogs scroll with the side buttons.
- **The same GitHub updater in the Web UI** — the browser Firmware Update page can run the same release check and
  validated OTA installation, with a scrollable changelog and confirmation controls. Existing local firmware upload
  and on-device SD-card installation remain available.
- **Release-aware version and asset selection** — numbered E-inx suffixes such as `w2` and `3_update` compare in
  release order, and the updater prefers the release workflow's `firmware.bin` while accepting named E-inx firmware
  assets.

## What's New in E-inx 1.3.17-w2

- **Smooth first page turn after wake** — X3 wake initialization now performs one guarded full synchronization plus
  the conditioned differential settling pass, avoiding a redundant hard-flash refresh on the first page turn.
- **Reading-minutes status items** — four new status-bar choices show session minutes; session/goal;
  session/today/goal; or session/today.

- **More accurate pages-read statistics** — quick page peeks and immediate backtracking no longer inflate the page
  count. EPUB and XTC readers now use a bounded, per-book adaptive dwell-time check, and page/chapter jumps count
  only pages actually read.

- **Experimental X3 OEM B/W reinforcement waveform** — opt-in, no-flash differential page transitions using the
  X3 OEM V5.6.33 `AA-pre-BW(mid)` waveform.
- **Independent targets** — separately enable reinforcement for image-free reader pages (including the B/W base
  beneath text anti-aliasing), explicitly marked monochrome UI transitions, and dithered
  Recent/Shelf/Statistics thumbnails. Reader `Auto` and forced `Fast` can use reinforcement; explicitly forced
  `Half` and `Full` remain strong cleanup requests.
- **Cleanup safeguards** — boot/reset, sleep, grayscale, dense-image, explicit refresh, and unknown-controller-state
  paths retain full/resync fallbacks. A separately configurable periodic full clean defaults to every 30 reinforced
  updates; `Full clean now` in Experimental settings and manual Page Refresh remain available.
- **Waveform provenance** — LUT bytes and controller-state behavior are adapted from
  [`open-x4-epaper/community-sdk` commit `198ad267`](https://github.com/open-x4-epaper/community-sdk/commit/198ad267219c25c8ab84418b806c66f1fb5216a3)
  and the MIT-licensed FreeInk/YACP X3 port.

## What's New in E-inx 1.2.17

E-inx 1.2.17 builds on Inx 1.0.17 with the following additions and improvements.

### New Features

- **News reader** — daily news tab with auto-download, bookmarking, and archiving. Downloads EPUB news files on a schedule using saved Wi-Fi credentials.
- **Dark mode** — full runtime dark mode with proper image tone preservation. Grayscale images keep their original tones while the UI inverts.
- **Stats dashboard** — the Recent page now shows a reading statistics overview: current book progress, daily reading time, 7-day averages, reading streak, and comparative stats against previous books.
- **Daily reading goals** — configurable daily reading time targets (15/30/45/60 min) with streak tracking, stored for up to two years of history.
- **Go to page** — navigate to a specific estimated page number in EPUBs, with a page slider in the menu drawer.
- **Edit metadata** — long-press a book in the Library to edit its title and author. Overrides persist across cache clears and sync to recent books and statistics.
- **If Found screen** — displays owner/contact info from `/if_found.txt` on the SD card.
- **Reading stats backup/restore** — export and import all per-book reading statistics as JSON to `/.backups/reading_stats/`.
- **Sleep cover repair** — one-tap regeneration of cached sleep cover images.
- **Firmware update web UI** — browser-based OTA update page with drag-and-drop upload, progress bar, image validation, and CSRF protection.
- **Hybrid sleep mode** — shows the current book cover when sleeping from the reader, custom sleep image otherwise.
- **Sleep image selection** — per-image include/exclude for random sleep image shuffle, now scanning both `/sleep/` and `/Wallpapers/`.
- **Sleep image rotation** — timer-based automatic sleep image cycling during deep sleep.
- **Power double-press gesture** — double-press the power button during sleep to advance to the next sleep image (configurable timing window).
- **Emergency restart** — hold the power button for 10 seconds from any screen to force a device restart.

### Reader Improvements

- **5-slot status bar** — expanded from 3 slots (left/middle/right) to 5 (left/inner-left/middle/inner-right/right).
- **Clock in status bar** — new TIME and SESSION_TIME status bar items showing current time and elapsed reading session time.
- **Reader refresh modes** — configurable refresh behavior per book: Auto, Fast, Half, or Full refresh.
- **Anti-ghosting** — experimental option that forces a half refresh when switching between activities to reduce ghosting.
- **Sunlight fading fix** — toggle to turn off the display between refreshes to reduce fading in direct sunlight.
- **Bottom bar clock** — optional clock display in the tab bar.
- **XTC reading stats** — statistics tracking now covers XTC/XTCH books in addition to EPUBs.
- **Reader presets** — preset format updated (v2 → v4) to include new status bar slots and refresh mode.
- **Book progress** — now stores estimated page number and page count alongside chapter/percentage progress.

### Library & UI Improvements

- **6-tab layout** — tabs expanded from 5 to 6: Recent, News, Library, Settings, Sync, Stats.
- **Long-press metadata editing** — long-press any book in Library to edit title/author.
- **Shelf view persistence** — shelf view mode is now preserved when leaving the Library tab.
- **Thumbnail auto-generation** — library indexing now automatically generates missing thumbnails for EPUB and XTC books.
- **Statistics** — book stats now index XTC books, apply metadata edits, and display daily reading duration.
- **Settings menu** — new entries for firmware update, If Found viewer, reading stats backup/restore, and sleep cover repair.
- **Clear cache** — new Reading Stats cache group; clearing book cache without clearing stats auto-backs-up and restores them.

### Display & Graphics Improvements

- **Dark mode rendering** — pixel-level BW inversion with image tone preservation via `SleepImageToneGuard`.
- **Anti-ghosting** — one-shot refresh mode override when switching activities or toggling dark mode.
- **Image error handling** — `ImageToneGuard` RAII class, dark-mode letterbox bars around contained images, proper cleanup on decode failure.
- **JPEG decode fixes** — improved error handling with per-MCU-row tracking; decode loop properly breaks on error.
- **Image cache** — version bumped (39 → 46) to include dark mode state in cache hash; corrupt entries auto-deleted; atomic writes via temp file + rename.
- **Thumbnail generation** — expanded to support PNG and BMP covers in addition to JPEG; failed thumbnails cleaned up.
- **Display HAL** — `displayBuffer` and gray buffer functions accept `turnOffScreen` parameter for sunlight fading fix; X3 half-refresh resync support.

### Network & Connectivity Improvements

- **Firmware OTA** — robust version parsing, pre-release version handling, ESP32-C3 image validation (magic byte, chip ID, size limits), TLS certificate checking restored.
- **HTTP redirects** — HTTP downloader follows up to 10 redirects.
- **Auto-connect** — Local Network activity can auto-connect using saved Wi-Fi credentials before showing the picker.
- **Wi-Fi cleanup fix** — fixed cleanup order in Wi-Fi selection to prevent use-after-free on exit.
- **Web server** — new firmware update endpoints (`/update`, `/api/update/status`, `/api/update/upload`), sleep image exclusion API, sleep/wake trace diagnostics.

### System & Stability Improvements

- **Sleep/wake trace** — diagnostic infrastructure recording 17 event types across deep sleep cycles, persisted to SD card for crash analysis.
- **Atomic file writes** — EPUB extraction, image cache, settings, and sleep trace all use temp file + rename for crash safety.
- **ZIP write verification** — `readFileToStream()` now checks write return values and fails on short writes.
- **EPUB namespace handling** — OPF parser now handles arbitrary XML namespace prefixes for Dublin Core metadata (fixes parsing of some EPUBs).
- **EPUB metadata override** — persistent title/author overrides stored in `<cache>/meta.override`, applied automatically on load.
- **Book progress compatibility** — forward-compatible loading of progress files from older versions.
- **Input flush** — new `flush()` method re-baselines button state and clears pending events.
- **Boot resilience** — resume path falls back to RTC-retained path; OTA image confirmed on first boot after update.
- **Deep sleep** — split prepare/enter for checkpoint saving; X3 battery latch managed per wake mode for lower drain.
- **Power wake detection** — early boot gesture recognition with configurable timing, NVS persistence across sleep cycles, 3-second emergency boot escape.
- **Display buffer safety** — null-pointer guards on display clear and refresh prevent crashes when frame buffer allocation fails.
- **Settings migration** — robust upgrade path from E-inx 1.2.0-6 and upstream 1.0.17 settings files; field ordering preserves E-inx custom settings across version upgrades.
- **Boot diagnostics** — serial boot-stage logging with timestamps and heap usage for debugging startup issues.
- **Session state** — expanded with sleep timer fields, wake reason tracking, and forward-compatible version loading.

### Scripts & Utilities

- **`scripts/flash_firmware.py`** — USB firmware flasher with native file picker, serial port auto-detection, ESP32-C3 image validation, and safe/app-only modes.
- **`scripts/azw3_to_epub.py`** — AZW3/MOBI/KF8 to EPUB converter using Calibre, with image cropping, BW e-reader optimization, and guided crop extraction.
- **`scripts/xteink_x3_batch_converter.py`** — batch image converter for 528×792 sleep/wallpaper images with crop/contain/stretch modes and Floyd-Steinberg dithering.

### Build & CI

- **GitHub Actions** — release workflow now publishes firmware assets (`firmware.bin`, `bootloader.bin`, `partitions.bin`) via `softprops/action-gh-release@v2`.

### Documentation

- **[Firmware updates](docs/firmware-updates.md)** — complete guide covering Web UI, Online OTA, and USB flash workflows with HTTP API reference.
- **[Web server endpoints](docs/webserver-endpoints.md)** — API documentation for firmware update endpoints.

---

## What You Can Do

- Read **EPUB**, **XTC / XTCH**, **TXT**, and **MD** files.
- Browse books from **Recent**, **News**, **Library**, **Settings**, **File Transfer**, and **Statistics** tabs.
- Use EPUB features such as bookmarks, annotations, go-to-page, go-to-percent, table of contents, footnotes, per-book settings, and KOReader sync.
- Edit book metadata (title, author) and manage favorites from the Library.
- Render **JPEG**, **PNG**, and **BMP** images directly.
- Use **1-bit** or **2-bit** image rendering.
- Enable **dark mode** for light-on-dark reading.
- Cache rendered images and system data for faster repeat loads.
- Use custom sleep screens, recent-book sleep screens, transparent cover sleep screens, hybrid sleep screens, or date/time sleep screens on supported devices.
- Shuffle sleep images with per-image include/exclude, timer rotation, and power-button advance.
- Track daily reading goals and streaks.
- Backup and restore reading statistics.
- Install reader fonts from the SD card instead of baking large fonts into firmware.
- Connect to Wi-Fi, Calibre, OPDS catalogs, KOReader sync, and the local web file manager.
- Update firmware from the browser, over-the-air, or via USB.
- Tune reader layout, buttons, fonts, status bar, refresh behavior, image quality, and display options.

## Main Features

### Reader

- EPUB paging with saved progress.
- EPUB layout support for tables, drop caps, borders, images, lists, blockquotes, superscript/subscript, and common CSS spacing/alignment.
- EPUB text annotation and highlight support.
- EPUB bookmarks.
- Go to a specific page or percentage in an EPUB.
- Table of contents, bookmark, annotation, and footnote navigation from the in-book menu.
- EPUB menu tools for deleting cache/progress, deleting a book, generating full data, and regenerating thumbnails.
- Per-book reader settings and reader presets.
- Configurable reader refresh mode (Auto, Fast, Half, Full).
- 5-slot status bar with time and session time display.
- Reading statistics with daily goals and streak tracking.
- KOReader sync support.
- TXT / MD reader.
- XTC / XTCH reader with chapter selection.
- Auto page turn support for EPUB and XTC reading.

### Images

- Native JPEG rendering with improved error handling.
- Native PNG rendering.
- BMP rendering.
- 1-bit and 2-bit image modes.
- Low, medium, and high image quality options for reader images.
- Low, medium, and high sleep image quality options.
- Display cache for faster repeated image draws (dark-mode-aware).
- Improved image scaling and dithering.
- Dark-mode letterbox bars for contained images.
- Cover, thumbnail, and sleep-screen rendering options.
- Thumbnail generation for EPUB and XTC books (JPEG, PNG, and BMP covers).

### Library

- Recent books page with stats dashboard.
- News reader tab with auto-download.
- Folder-based library browser.
- Flat all-books view.
- Cover shelf view for EPUB and XTC books (persisted on exit).
- Tag view when the library index is enabled.
- Favorites.
- Long-press metadata editing.
- Sort options by title, group/folder, reading state, and tag.
- Optional indexed library mode for faster browsing and tag management.
- Auto-generation of missing thumbnails during indexing.
- List and grid library modes.

### Display

- Text anti-aliasing.
- Dark mode with image tone preservation.
- Sunlight fading fix.
- Anti-ghosting (experimental).
- Configurable refresh frequency.
- Optional half refresh when opening main tabs.
- Sleep screen modes:
  - Dark
  - Light
  - Custom image
  - Recent book
  - Transparent cover
  - Hybrid (cover from reader, custom otherwise)
  - None
  - Date/time on supported devices
- Custom sleep images from `/sleep/`, `/Wallpapers/`, `/sleep.bmp`, `/sleep.jpg`, or `/sleep.jpeg`.
- Per-image shuffle selection, timer-based rotation, and power-button image advance.

### Sync & Network

- Join Wi-Fi networks (with auto-connect from saved credentials).
- Create a hotspot.
- Connect to Calibre.
- Browse OPDS catalogs.
- Use KOReader sync.
- Upload files through the local web interface.
- Update firmware from the browser-based update page.
- Over-the-air updates with image validation.

### Settings

Settings are split into simple **System** and **Reader** panels.

System settings include:

- Sleep screen and hybrid mode.
- Sleep image picker with shuffle selection.
- Sleep image rotation timer.
- Power button double-press image advance.
- Recent page mode.
- Library mode.
- Button layout.
- Power button behavior.
- Time to sleep.
- Dark mode.
- Sunlight fading fix.
- Anti-ghosting (experimental).
- Bottom bar clock.
- Daily reading goal.
- Library indexing.
- Library custom sort.
- Cache clearing (with reading stats protection).
- Thumbnail generation.
- Reading stats backup and restore.
- If Found viewer.
- KOReader, OPDS, Calibre, and OTA update tools.
- About page with device memory information.

Reader settings include:

- Font family and size.
- SD-card font families.
- Line height and word spacing.
- Screen margins.
- Paragraph alignment.
- CSS indentation.
- Reading orientation.
- Hyphenation.
- Bionic Reading.
- Page navigation mapping.
- Long-press chapter or page skipping.
- Auto page turn.
- Text anti-aliasing.
- Image grayscale / 2-bit rendering.
- Reader refresh mode (Auto, Fast, Half, Full).
- Smart refresh on image-heavy pages.
- Status bar layout (5 slots: left, inner-left, middle, inner-right, right).

## Web Interface

The local web interface includes:

- **Dashboard**: device status, IP address, Wi-Fi strength, memory, uptime, and quick links.
- **Files**: browse folders, upload files, create folders, delete files, upload cover art to `/sleep`, and set folder thumbnails.
- **Epub**: drag-and-drop EPUB imports, folder creation, JPEG optimization, optional packaged device thumbnails, and import progress.
- **Tags**: create reusable tags and assign them to indexed books.
- **Fonts**: build SD-card font packs from TTF/OTF files and upload them to `/fonts`.
- **Settings**: edit system settings, reader settings, Wi-Fi networks, KOReader settings, and OPDS servers.
- **Firmware Update**: drag-and-drop firmware upload with progress, image validation, and OTA partition info.

## Fonts

E-inx includes built-in **Literata** and **Atkinson Hyperlegible** reader fonts.

You can also install fonts on the SD card:

```text
/fonts/
  MyFont/
    Regular_10.bin
    Regular_12.bin
    Regular_14.bin
    Bold_14.bin
    Italic_14.bin
    BoldItalic_14.bin
```

The web font manager converts TTF/OTF files into the `.bin` format used by the reader. Regular is required; bold, italic, and bold italic are optional.

## Custom Sleep Images

Put sleep images on the SD card:

```text
/sleep/
  image1.bmp
  image2.jpg
  image3.png

/Wallpapers/
  wallpaper1.bmp
  wallpaper2.jpg

/sleep.bmp
/sleep.jpg
/sleep.jpeg
```

You can choose a fixed sleep image from settings, let the device pick one randomly with per-image include/exclude, or enable timer-based rotation during sleep. Double-pressing the power button can also advance to the next image.

## Cache

E-inx uses SD-card cache files to save RAM and speed up repeated work.

Main cache locations:

```text
/.metadata/       EPUB metadata, layout, progress, stats, annotations
/.metadata/xtc/   XTC / XTCH metadata and progress
/.backups/        Reading stats backup (JSON)
/.system/cache/   Display and image cache
/.system/         Settings, TXT cache, sleep/wake traces, and system data
/fonts/           SD-card reader fonts
/sleep/           Custom sleep images
/Wallpapers/      Additional sleep images
```

You can clear cache from **Settings -> Actions -> Delete Cache**. Reading stats can be preserved separately during cache clears.

Deleting `/.metadata` will force EPUB layout data to be rebuilt.

## Installing

### Update or flash

- **From the E-inx web UI:** open **File Transfer → Firmware Update**, then upload `firmware.bin` at the `/update` URL shown by the reader.
- **Online OTA:** open **Settings → Device Actions → Check for updates** and choose **Online update**.
- **Local USB tool:** run `python3 scripts/flash_firmware.py` for a native file picker, automatic serial-port detection, image validation, and safe OTA-slot reset.
- **CrossPoint browser flasher:** use [crosspointreader.com/#flash-tools](https://crosspointreader.com/#flash-tools) with **Custom .bin**.

See [Firmware updates](docs/firmware-updates.md) for the complete workflow, safety checks, automation options, and API.

To return to official firmware, use the supported CrossPoint flash/recovery tools. USB-locked devices should keep a working WiFi or SD update path available.

## Development

### Requirements

- PlatformIO Core (`pio`) or VS Code with PlatformIO.
- Python 3.8 or newer.
- USB-C cable.
- SDL2 for simulator builds.

### Clone

```sh
git clone --recursive https://github.com/rttgnck/E-inx
cd E-inx
```

If you already cloned without submodules:

```sh
git submodule update --init --recursive
```

### Build

```sh
pio run
```

### Flash

```sh
pio run --target upload
```

Or use the USB flash script:

```sh
python3 scripts/flash_firmware.py
```

### Web Assets

The firmware embeds the HTML and JS from `src/network/html` and `data/js`.

```sh
python3 scripts/build_html.py
```

`pio run` also regenerates these files before compiling.

### Simulator

E-inx includes two native simulator targets based on the CrossPoint simulator.

For the full SDL/device UI simulator:

```sh
CROSSPOINT_SIM_SD=./fs_ pio run -e simulator -t run_simulator
```

For dashboard-only testing:

```sh
CROSSPOINT_SIM_SD=./fs_ pio run -e simulator_web -t run_simulator
```

The simulator stores its SD-card data in the folder passed through `CROSSPOINT_SIM_SD`. The firmware web server is exposed at `http://127.0.0.1:8080/` when the simulated device starts a hotspot or local network server.

On macOS, SDL2 is required:

```sh
brew install sdl2
```

For more simulator details, see the [CrossPoint simulator project](https://github.com/crosspoint-reader/crosspoint-simulator).

### Serial Debugging

Install the monitor dependencies:

```sh
python3 -m pip install pyserial colorama matplotlib
```

### Utility Scripts

- **`scripts/flash_firmware.py`** — USB firmware flasher with file picker, serial auto-detection, and image validation.
- **`scripts/azw3_to_epub.py`** — Convert AZW3/MOBI/KF8 files to EPUB with optional e-reader image optimization.
- **`scripts/xteink_x3_batch_converter.py`** — Batch convert images to 528×792 sleep/wallpaper format with dithering.

Run the serial monitor:

```sh
# Linux
python3 scripts/debugging_monitor.py

# macOS example
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

## Contributing

Contributions are welcome.

1. Fork the repository.
2. Create a branch.
3. Make your changes.
4. Open a pull request.

## Credits

E-inx is based on [Inx](https://github.com/obijuankenobiii/inx) by obijuankenobiii, which is itself a fork of CrossPoint.
