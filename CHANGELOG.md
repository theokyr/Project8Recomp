# Changelog

## v0.2.1 — accepted Steam Deck frontier refresh

This patch release moves the public build from the p0p22 runtime configuration
shipped in v0.2.0 to the accepted p0p39 Steam Deck frontier. Packaging,
supported disc identity, executable names, save locations, and launcher flow
are unchanged.

### Adjacent texture descriptor-set reuse

The Performance preset now enables one additional default-off Vulkan path. It
reuses a texture descriptor set only when the immediately preceding request in
the same submission has the exact same descriptor-set layout and ordered image
views, layouts, and samplers. Submission and transient-pool boundaries clear
the entry, and every mismatch follows the original allocation/write path.

On the deterministic 2,600–2,799-draw Funpark fixture, the path matched
**39.17%** of non-empty texture stages and avoided about **1,001 descriptor
writes per frame**. A same-binary, cool-start six-run gate moved the
median-of-three mean from **25.630 to 25.332 ms (-1.16%)**, effective FPS from
**39.02 to 39.48**, with process CPU flat at roughly **257%**. The wider
hash-table design was not shipped: although it found another 450 exact sets per
frame, it regressed mean frame time by 0.21% in all three ordered comparisons.

The patch inventory now contains 36 entries. The three new source patches add
the accepted adjacent reuse path, default-off sequence capture used to measure
it, and reproducible source-path mapping; capture is inactive during normal
play.

### Player-facing wording

The launcher now says directly that the port contains no game content and will
never download any. This is a wording clarification only: setup still reads a
disc image supplied from the player's own copy, writes the extracted data into
the portable install, and sends nothing elsewhere.

## v0.2.0 — Steam Deck performance and release packaging

v0.2.0 promotes the runtime configuration measured on Steam Deck, fixes the
resolution control that v0.1.0 exposed but did not actually apply, and replaces
launcher-only CI archives with a process that assembles and tests complete
release archives before tagging.

### One player entry, without breaking old installs

- The portable ZIP adds one executable: `Project8Recomp`
  (`Project8Recomp.exe` on Windows). Run it once for setup; after setup it asks
  the existing launcher to Play immediately. `--gui` opens the launcher home
  screen instead.
- It is a wrapper, not a second launcher and not a shortcut around one. Setup,
  saved launch settings, single-instance checks, the game supervisor, and crash
  cleanup still run through `thps_p8_gui`.
- Every v0.1.0 `thps_p8*` executable remains present under the same name and
  with the same role. Existing Steam shortcuts and scripts continue to work.
- Linux and Windows ship as ZIP files with `BUILDINFO.txt`, per-file
  `SHA256SUMS`, the changelog, licences, assets, and all required runtime
  components. A Deck user can unpack the Linux ZIP and add `Project8Recomp` as
  a Non-Steam Game without forcing Proton.
- The Windows ZIP includes the app-local Visual C++ runtime used by its `/MD`
  binaries, so a clean player machine does not need a separate redistributable
  installer before the wrapper can start.

### Resolution changes now take effect — resolves #2

The launcher previously saved and emitted a resolution choice while the
runtime remained in its default borderless-fullscreen mode. Borderless
fullscreen always uses the desktop dimensions, so the requested window size
was discarded and every selection looked identical.

- Choosing a size now implies windowed mode when Fullscreen is left at Game
  default. An explicit Fullscreen choice still takes precedence.
- Named 16:9 modes continue through the runtime's `--resolution` option.
- 1280x800, which is not an original Xbox 360 preset, sets the host window and
  guest video-mode dimensions directly for the Steam Deck panel.
- Argument-rendering tests cover named modes, 1280x800, implicit windowed mode,
  and explicit-fullscreen precedence.

### Windows pacing and CPU use — mitigates #1

The v0.1.0 runtime disabled SDL's Windows timer resolution. A 16.67 ms vblank
deadline could therefore wake at 31.25 ms, after which the worker delivered one
vblank and discarded the other elapsed interval. The guest could be paced near
32 Hz while threads waiting for that counter continued consuming CPU—the same
shape reported in #1.

v0.2.0 uses a high-resolution deadline timer on Windows, keeps SDL's timer
resolution enabled, and performs bounded vblank catch-up after a late wake.
Several other hot waits now round sleep durations correctly or block on the
counter/event that can actually satisfy them.

The complete Windows archive was exercised through Proton 10 on a Steam Deck.
In a short menu window it measured **16.68 ms p50 / 17.71 ms p95**. In the
marked 2,600-plus-draw Funpark window it sustained the same 30 fps presentation
mode as native (**33.30 / 34.45 ms p50 / p95**). Uncapped, the Windows build
under Proton measured **28.00 / 32.15 ms** against native Linux at **25.00 /
27.99 ms**. The detached Windows process used roughly **319–349% CPU** across
the two Funpark runs, compared with **253–256%** for native Linux. That is a
real compatibility-layer cost, but the process remained bounded to about 3.5
of the Deck's eight logical cores and did not reproduce v0.1.0's near-32 Hz
pacing failure. Real Windows remains explicitly unverified, so this release
calls #1 mitigated rather than claiming a fix.

### Accepted Steam Deck performance frontier

The release enables the measured title configuration by default. It combines
command-processor parsing and primitive-cache fixes with bounded guest waits,
native vertex/index residency and unpack paths, upload prefetch, and exact
reuse of adjacent sampler, texture-request, and Vulkan view work. Every lever
can still be disabled through the launcher's Performance setting.

On the deterministic late-game Funpark fixture, the accepted Steam Deck LCD
build's three-run uncapped promotion median measured approximately 2,667 draws
per frame, **25.307 ms mean**, **25.000 ms p50**, **27.015 ms p95**, **39.51
effective FPS**, and **257.1% process CPU** (about 2.57 logical cores). With the
player-default frame cap, the release-candidate archive held a stable 30 FPS
presentation mode in the same severe view at **33.34 ms p50 / 33.52 ms p95**.
This is the current heavy-scene frontier, not a locked-60 claim. Smaller
improvements were retained when they passed correctness and showed no
regression.

### Reproducible release gates

- The exact 33-patch SDK order is committed in
  `patches/rexglue-sdk/series` and round-trips against the pinned SDK base.
- The publishable-tree check now includes tracked and untracked non-ignored
  files. Root-only ignore rules no longer hide `src/game/` from Git.
- Every candidate commit builds launcher artifacts on Ubuntu 24.04 and Windows
  Server 2022. The maintainer downloads those exact-commit artifacts, combines
  them locally with the prepared game/runtime components, checks the Linux
  GLIBC 2.39 / GLIBCXX 3.4.32 ceiling and redistribution boundary, and smoke
  tests the complete Linux and Windows ZIPs on Deck before any tag exists.

## v0.1.0 — first public release

The port is playable start to finish on Linux and macOS from a copy of the game
you own.

### Playing

- **Launcher.** Point it at your disc image and it does the rest: checks the
  disc, copies the game, and starts it. After the first run it is a Play button.
- **Controller support** throughout the launcher and the game, including
  plugging one in after the launcher is already open.
- **Display settings** — resolution, which monitor, windowed or fullscreen, and
  the frame rate cap — saved in `config/settings.toml`.
- **Portable install.** Everything lives in one folder. Move it, copy it between
  your own machines, delete it — nothing is written anywhere else.

### Refusing bad input

- The disc is identified by hashing `default.xex` inside the image, before
  anything is copied. A wrong disc costs a moment rather than 4.7 GB.
- The game refuses to start against absent, wrong or incomplete game data
  however it is launched, instead of crashing partway through startup.
- Free space is checked before the copy starts, not discovered during it.
- Setup can be stopped, and stopping it removes what had been copied.
- A truncated or malformed disc image produces a verdict rather than a crash.

### Performance

Roughly 100 fps uncapped on a desktop Linux machine with a discrete GPU, up from
24.5 at the start of the project. Three host-runtime patches account for it;
they are in `patches/rexglue-sdk/`.

macOS runs natively on Apple Silicon and is functionally correct but much
slower — see [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).

### Platforms

| Platform | Status |
| --- | --- |
| Linux x86_64 | Verified |
| macOS arm64 | Verified |
| Windows x86_64 | Complete archive, experimental; later reported to start on Windows 10 with unusable performance |

### Known issues

In-level cutscenes run too fast if the frame rate cap is turned off. Leave it
on, which is the default. Full list in
[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).
