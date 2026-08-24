# Changelog

## v0.3.0: ReXGlue 0.10 and a faster Steam Deck build

v0.3.0 moves the port to ReXGlue 0.10 and carries the title-specific runtime
work onto the new SDK. The result is a substantial improvement in the heaviest
repeatable scene we use for Steam Deck testing, lower process CPU use, working
Apple Silicon presentation with the current runtime, and a refreshed Windows
build. The supported disc, save and settings locations, setup flow, and all
existing executable names remain unchanged.

### Steam Deck performance

The exact native Linux release build was tested in a deterministic late-game
Funpark view averaging about 2,665 draws per frame. Compared with the v0.2.1
release archive on the same Steam Deck LCD and fixture:

| Metric | v0.2.1 | v0.3.0 | Change |
| --- | ---: | ---: | ---: |
| Mean frame time | 25.00 ms | **21.94 ms** | **-12.2%** |
| Effective FPS | 40.01 | **45.58** | **+13.9%** |
| p50 / p95 | 25.00 / 27.00 ms | **22.00 / 23.00 ms** | lower and tighter |
| Process CPU | 257.6% | **223.1%** | **-34.5 points** |

A separate six-run, cool-start comparison of the complete old and new runtime
stacks confirmed the result. Mean frame time improved by 12.34%, effective FPS
by 14.08%, and process CPU by 13.6%, with the new build winning all three
ordered comparisons. The scene, draw work, launch settings, and screenshot
checkpoints matched between both arms.

These are uncapped heavy-scene measurements, not a locked-60 claim. The normal
player setting still caps the game to preserve correct cutscene timing.

### ReXGlue 0.10 migration

The game manifest, generated-code integration, input hooks, and runtime patches
have been updated for ReXGlue 0.10. The new public patch series starts from the
upstream v0.10.0 tag and contains two self-contained patches: the complete
Project 8 runtime stack and the required MoltenVK 1.4.2 update. This replaces
the 37 incremental patches used by v0.2.1 and makes a fresh SDK reconstruction
shorter and easier to audit.

The accepted performance paths remain enabled by the launcher's Performance
preset. They cover command processing, native vertex and index handling,
resource residency and prefetch, repeated texture and sampler work, and bounded
guest waits. Each path still has an individual runtime switch for debugging.

### Apple Silicon presentation

The first ReXGlue 0.10 test build produced audio and valid internal screenshots
while the macOS window stayed black. This was traced to the presentation layer,
and the release now bundles MoltenVK 1.4.2. A fresh marked run on a 2020 Apple
M1 MacBook Pro rendered the complete Funpark view correctly at about 2,668
draws per frame, and its graphics were visually confirmed on the host display.

This is a correctness fix, not a performance claim. That severe scene averaged
**241.64 ms / 4.14 effective FPS**, with **235.96 ms p50** and **264.60 ms
p95**. Apple Silicon remains too slow for comfortable play in busy areas. CPU
is not reported because the fixture's process accounting is Linux-specific.

The archive remains self-contained, ad-hoc signed, and not notarized. Players
do not need Homebrew, but still need the documented right-click, **Open** step
the first time they launch it.

### Windows status and issue #1

The complete Windows release build was unpacked and exercised through Proton
10 in Steam Deck Game Mode. The launcher handoff and Vulkan path worked, and
all nine marked Funpark checkpoints rendered. The player-default path measured
**33.32 / 34.25 ms p50 / p95**, holding its intended 30 FPS presentation.
Uncapped, it measured **27.55 ms mean / 36.30 effective FPS / 27.01 ms p50 /
30.99 ms p95**.
That is a small, non-regressing improvement over the v0.2.1 Proton result.

The high-resolution Windows pacing and bounded wait fixes introduced after
v0.1.0 remain in place, so this continues to mitigate the excessive CPU and
below-speed behaviour reported in issue #1. Proton reparents the game outside
the test supervisor, which makes its process-CPU reading invalid. Real Windows
hardware is still unverified, so v0.3.0 does not claim that the original report
is fully resolved.

### Packaging and compatibility

- Linux, macOS, and Windows continue to ship as complete portable archives.
- `Project8Recomp` remains the recommended entry: it opens setup on a new
  install, goes to Play after setup, and accepts `--gui` to show the launcher.
- The four original `thps_p8*` executable names remain present for existing
  shortcuts and installs.
- The launcher continues to expose **Open save folder** and **Open logs folder**
  buttons so backups and issue reports do not require finding hidden paths.
- Release archives remain game-content-free and include file checksums,
  licences, build information, the changelog, and all platform runtime files.

## v0.2.1: Steam Deck performance refresh

This patch release pushes Steam Deck performance further in busy scenes. The
exact Linux release archive averaged **25.00 ms per frame / 40.01 effective
FPS** in a deterministic Funpark view with roughly 2,667 draws per frame. It
also restores a complete Apple Silicon archive and improves the launcher's
player-facing guidance. The supported disc identity, existing executable
names, save locations, and launcher flow are unchanged.

### Adjacent texture descriptor-set reuse

The Performance preset now enables one additional default-off Vulkan path. It
reuses a texture descriptor set only when the immediately preceding request in
the same submission has the exact same descriptor-set layout and ordered image
views, layouts, and samplers. Submission and transient-pool boundaries clear
the entry, and every mismatch follows the original allocation/write path.

On the deterministic 2,600-2,799-draw Funpark fixture, the path matched
**39.17%** of non-empty texture stages and avoided about **1,001 descriptor
writes per frame**. A same-binary, cool-start six-run gate moved the
median-of-three mean from **25.630 to 25.332 ms (-1.16%)**, effective FPS from
**39.02 to 39.48**, with process CPU flat at roughly **257%**. The wider
hash-table design was not shipped: although it found another 450 exact sets per
frame, it regressed mean frame time by 0.21% in all three ordered comparisons.

The patch inventory now contains 37 entries. The four new source patches add
the accepted adjacent reuse path, default-off sequence capture used to measure
it, reproducible source-path mapping, and a usable clang-cl warning level;
capture is inactive during normal play.

### Steam Deck launches match the measured path

SteamOS includes RenderDoc's Vulkan loader as a system library. The runtime
probes for that library by name, so the v0.2.0 portable ZIP could attach the
debugger during an ordinary player launch even though no capture was requested.
That adds substantial per-draw overhead in the scenes where the Deck needs the
performance work most.

The Linux archive now carries a dependency-free guard library, and both
`Project8Recomp` and the existing GUI launcher put it first for the game handoff.
This makes the normal portable launch match the no-debugger condition used for
the published Steam Deck measurements. Developers can still request the system
RenderDoc library explicitly with `THPS_P8_RENDERDOC=1`.

### Player-facing wording

The launcher now says directly that the port contains no game content and does
not download any. This is a wording clarification only: setup still reads a
disc image supplied from the player's own copy, writes the extracted data into
the portable install, and sends nothing elsewhere.

The launcher copy has also been tightened across setup, settings, recovery, and
error states. Its home screen now has separate **Open save folder** and **Open
logs folder** buttons, and the issue forms direct reporters to the latter. Paths
and display names are escaped before insertion into RML, so characters such as
`<` and `&` in a local filename remain text instead of being parsed as markup.

### Complete Apple Silicon release restored

v0.2.1 again ships a complete `macos-arm64` archive, restoring the platform
package that v0.1.0 had and v0.2.0 omitted. The archive now carries the Vulkan
loader and MoltenVK itself, uses their adjacent manifest even when started from
Finder, and ad-hoc signs every Mach-O file after its install names and symbols
are normalised. A player does not need Homebrew; the archive is self-contained,
but it remains neither Developer ID-signed nor notarized, so the documented
right-click → Open step still applies.

The exact release candidate was exercised on a 2020 Apple M1 MacBook Pro. Its
packaged supervisor started the game, the bundled Vulkan loader and MoltenVK
were loaded, and the marked fixture reached a visually verified Funpark frame
at 2,704 draws. This restores functional support, but it is not a performance
claim. Its 2,500-plus-draw rows averaged
**236.79 ms / 4.22 effective FPS** (233.75 ms p50, 251.29 ms p95), while the
process used roughly **317% CPU** during the run. Apple Silicon remains too
slow for comfortable play in that severe scene.

### Exact release archives exercised

The complete Linux and Windows release candidates were unpacked and tested on
a Steam Deck in Game Mode, using the same marked Funpark view as the performance
work. The Linux archive averaged **24.996 ms / 40.01 effective FPS**, with
24.998 ms p50, 27.003 ms p95, roughly 2,667 draws per frame, and **257.6%
process CPU**. The Windows archive rendered all nine expected checkpoints
through Proton 10. It held the player-default 30 FPS presentation mode at
**33.31 / 34.49 ms p50 / p95**; uncapped, it measured **27.83 / 32.02 ms p50 /
p95**, or 35.64 effective FPS.

Proton reparents the Windows game outside the test supervisor, so this run does
not provide a trustworthy process-CPU comparison. These results prove the
archive, launcher handoff, Vulkan path, scene loading, and pacing under Proton.
They still do not substitute for a test on real Windows hardware.

## v0.2.0: Steam Deck performance and release packaging

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

### Resolution changes now take effect (resolves #2)

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

### Windows pacing and CPU use (mitigates #1)

The v0.1.0 runtime disabled SDL's Windows timer resolution. A 16.67 ms vblank
deadline could therefore wake at 31.25 ms, after which the worker delivered one
vblank and discarded the other elapsed interval. The guest could be paced near
32 Hz while threads waiting for that counter continued consuming CPU. This is
the same shape reported in #1.

v0.2.0 uses a high-resolution deadline timer on Windows, keeps SDL's timer
resolution enabled, and performs bounded vblank catch-up after a late wake.
Several other hot waits now round sleep durations correctly or block on the
counter/event that can actually satisfy them.

The complete Windows archive was exercised through Proton 10 on a Steam Deck.
In a short menu window it measured **16.68 ms p50 / 17.71 ms p95**. In the
marked 2,600-plus-draw Funpark window it sustained the same 30 fps presentation
mode as native (**33.30 / 34.45 ms p50 / p95**). Uncapped, the Windows build
under Proton measured **28.00 / 32.15 ms** against native Linux at **25.00 /
27.99 ms**. The detached Windows process used roughly **319-349% CPU** across
the two Funpark runs, compared with **253-256%** for native Linux. That is a
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

On the deterministic late-game Funpark fixture, the v0.2.0 Steam Deck LCD
build's three-run uncapped median measured approximately 2,667 draws
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

## v0.1.0: first public release

The port is playable start to finish on Linux and macOS from a copy of the game
you own.

### Playing

- **Launcher.** Point it at your disc image and it does the rest: checks the
  disc, copies the game, and starts it. After the first run it is a Play button.
- **Controller support** throughout the launcher and the game, including
  plugging one in after the launcher is already open.
- **Display settings:** resolution, monitor, windowed or fullscreen mode, and
  the frame rate cap, saved in `config/settings.toml`.
- **Portable install.** Everything lives in one folder. Move it, copy it between
  your own machines, or delete it. Nothing is written anywhere else.

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
slower. See [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).

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
