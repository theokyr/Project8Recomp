// What the launcher passes to the game, and where it remembers it.
//
// Before this, the launcher passed exactly three flags and every session opened
// a 720p borderless window on the primary display. That is the SDK's default
// and it matches `make play` with no knobs, so it was never a bug - but it was
// the most visible gap in the product, because the one thing a player expects a
// launcher to own is which screen the game opens on and how big it is.
//
// The whole surface is four choices. It is deliberately not a mirror of the
// game's cvar list: `--draw_resolution_scale_*`, `--video_mode_refresh_rate`
// and the residency toggles are measurement instruments, and a settings screen
// that exposes them would be a settings screen written for the people who
// already know how to pass a flag.
//
// ---------------------------------------------------- the omit-empty rule
//
// The hard rule this file exists to enforce:
//
//     A flag with no value is NOT emitted as `--flag=`. It is omitted.
//
// The Makefile records what the alternative costs, having paid it: a recording
// ended up with a mark literally named `--debug_panel_true`, because an empty
// `--flag=` consumed the next argv entry as its value. Every renderer that
// builds a command line from optional fields has to make this choice, and it is
// invisible until something downstream shifts by one argument.
//
// `RenderArgv` is therefore a pure function over a plain struct, and it is the
// only place in the launcher that turns a setting into a flag - so the rule has
// exactly one place to be wrong, and one test to hold it right.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace thps {

// A display as the player is offered it. `index` is SDL's ordinal, which is
// what the game's `--monitor` flag counts in.
struct DisplayOption {
  int index = 0;
  std::string name;  // e.g. "DP-1 (2560x1440 @ 144 Hz)"
};

struct Settings {
  // Empty means "whatever the game defaults to", which is the honest
  // representation of an unset preference and renders as no flag at all.
  std::string resolution;   // "", "720p", "1080p", "1440p", "4k"
  int monitor = -1;         // -1 = unset
  int fullscreen = -1;      // -1 unset, 0 false, 1 true  (tri-state on purpose)
  int vsync = -1;           // -1 unset, 0 false, 1 true

  // The presets offered, in the order they are offered. Kept beside the struct
  // so the UI and the validator cannot disagree about what is selectable.
  static const std::vector<std::string>& ResolutionChoices();

  // True when `resolution` is one of the choices or empty. A settings file is
  // user-editable text; a hand-typed "1081p" must not reach the game's argv.
  bool Valid() const;

  // Drops anything Valid() rejects back to unset. Called after loading, so a
  // corrupt or hand-edited file degrades to defaults instead of refusing to
  // start.
  void Sanitise();
};

// The three flags that are not settings and are never optional:
//
//   --game_data_root   where the extracted game lives
//   --mnk_mode=true    keyboard input is registered but DISABLED without it,
//                      so every key silently does nothing and Escape stops
//                      working.
//   --gpu_plugin=xenos without it the game starts, opens a window, runs, and
//                      draws nothing. A black screen reads as a broken port
//                      rather than a missing flag - this is the defect the
//                      owner's first walkthrough found.
//
// Plus the breadcrumb, so the next launch can say something useful if this one
// dies before drawing anything.
struct LaunchPaths {
  std::filesystem::path launcher_exe;
  std::filesystem::path game_data_root;
  std::filesystem::path breadcrumb;
};

// The whole command line, required flags and settings together. Pure: no
// filesystem access, no globals, so the test can assert on it directly.
std::vector<std::string> RenderArgv(const LaunchPaths& paths, const Settings& settings);

// config/settings.toml. Hand-written and hand-parsed - four scalars from a
// producer in this same binary, where a TOML dependency would cost more than it
// protects. Unknown keys are ignored rather than rejected, so a file written by
// a newer build still loads in an older one.
bool LoadSettings(const std::filesystem::path& file, Settings* out);
bool SaveSettings(const std::filesystem::path& file, const Settings& settings);

// Serialised form, exposed so the test can check a round trip without touching
// a disk.
std::string SerialiseSettings(const Settings& settings);
Settings ParseSettings(const std::string& text);

}  // namespace thps
