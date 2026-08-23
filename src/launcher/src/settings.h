// What the launcher passes to the game, and where it remembers it.
//
// The SDK defaults to borderless fullscreen at the desktop's dimensions. A
// requested size has no visible effect in that mode, so RenderArgv makes an
// otherwise-unset fullscreen choice windowed whenever a size is selected.
// Explicit fullscreen still wins. This is the one dependency between display
// settings, and it exists so the control cannot truthfully save a value and
// then visibly do nothing.
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
  // Kept as `resolution` in settings.toml for v0.1.0 compatibility. What the
  // player controls is window size: this title keeps its internal render
  // targets fixed regardless of the guest video mode.
  std::string resolution;   // "", "720p", "1080p", "1440p", "4k", "1280x800"
  int monitor = -1;         // -1 = unset
  int fullscreen = -1;      // -1 unset, 0 false, 1 true  (tri-state on purpose)
  int vsync = -1;           // -1 unset, 0 false, 1 true

  // Performance enhancements, as one choice rather than five.
  //
  // -1 unset (which RenderArgv treats as ON - see below), 0 off, 1 on.
  //
  // These are five separate cvars in the runtime, and they are deliberately
  // NOT exposed as five rows. `settings.h`'s rule is that this screen offers
  // choices a player can hold an opinion about, not a mirror of the cvar list;
  // nobody has an opinion about primitive_processor_cache_min_indices. What
  // they have an opinion about is "make it faster" and, if something looks
  // wrong, "stop doing that".
  //
  // Measured on a Steam Deck LCD 2026-08-21: 17.62 us/draw at the runtime's own
  // defaults against 11.78 with these on, and in play the heavy-draw areas move
  // from about 27 fps to about 40 in the busy Deck fixture.
  //
  // Unset means ON, which is the one place this file breaks its own
  // omit-empty symmetry, and it does so on purpose: the runtime's defaults are
  // off, a fresh install should be fast, and a player who has never opened this
  // screen should not be the only one running the slow configuration.
  int performance = -1;

  // The presets offered, in the order they are offered. Kept beside the struct
  // so the UI and the validator cannot disagree about what is selectable.
  static const std::vector<std::string>& ResolutionChoices();

  // The cvars `performance` stands for, and the value each takes when it is on.
  // One list, so the UI, the renderer and the test cannot disagree about what
  // the switch means.
  static const std::vector<std::pair<std::string, std::string>>& PerformanceFlags();

  // True when a choice names an explicit WxH guest video mode rather than one
  // of the game's named presets. The two render as different flags, so exactly
  // one place decides which a value is.
  static bool IsExplicitMode(const std::string& value, int* width, int* height);

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
