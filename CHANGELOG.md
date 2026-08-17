# Changelog

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
| Windows x86_64 | Launcher only, experimental, never run on real hardware |

### Known issues

In-level cutscenes run too fast if the frame rate cap is turned off. Leave it
on, which is the default. Full list in
[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).
