// Tests for the argv renderer and the settings round trip.
//
// A standalone binary with no framework: the launcher's other tests are Python
// (they drive the built executables), and pulling GoogleTest into a project
// whose whole point is a short link line would be a poor trade for four
// assertions. `ninja test-settings && ./test-settings` is the whole contract.
//
// The renderer is the thing under test because D16 is the rule most easily lost
// in a refactor and least visible when it breaks: an empty `--flag=` consumes
// the NEXT argv entry as its value, so the failure shows up as some unrelated
// flag going missing.

#include "../src/settings.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", what);
  ++g_failures;
}

bool Has(const std::vector<std::string>& argv, const std::string& flag) {
  return std::find(argv.begin(), argv.end(), flag) != argv.end();
}

// The failure D16 exists to prevent, stated as a predicate: no argument may be
// a flag with an empty value.
bool AnyEmptyValuedFlag(const std::vector<std::string>& argv) {
  for (const std::string& arg : argv) {
    if (arg.size() >= 3 && arg.rfind("--", 0) == 0 && arg.back() == '=') return true;
  }
  return false;
}

thps::LaunchPaths Paths() {
  thps::LaunchPaths paths;
  paths.launcher_exe = "/opt/thps/thps_p8_launch";
  paths.game_data_root = "/opt/thps/game";
  paths.breadcrumb = "/opt/thps/logs/last_launch.json";
  return paths;
}

void TestRequiredFlagsAlwaysPresent() {
  // All three are load-bearing and none is a user preference. --gpu_plugin in
  // particular: without it the game runs and draws nothing, which is the
  // failure that reads as a broken port rather than a missing flag.
  const std::vector<std::string> argv = thps::RenderArgv(Paths(), thps::Settings{});
  Check(argv.size() >= 5, "argv carries the launcher plus the required flags");
  Check(argv[0] == "/opt/thps/thps_p8_launch", "argv[0] is the supervisor");
  Check(Has(argv, "--game_data_root=/opt/thps/game"), "game data root is passed");
  Check(Has(argv, "--mnk_mode=true"), "mnk_mode is passed");
  Check(Has(argv, "--gpu_plugin=xenos"), "gpu_plugin is passed");
  Check(Has(argv, "--launcher-breadcrumb=/opt/thps/logs/last_launch.json"), "breadcrumb is passed");
}

void TestDefaultSettingsEmitNoDisplayFlags() {
  const std::vector<std::string> argv = thps::RenderArgv(Paths(), thps::Settings{});
  Check(!AnyEmptyValuedFlag(argv), "a default Settings emits no empty-valued flag (D16)");
  for (const char* flag : {"--resolution", "--monitor", "--fullscreen", "--vsync"}) {
    const bool present = std::any_of(argv.begin(), argv.end(), [flag](const std::string& a) {
      return a.rfind(flag, 0) == 0;
    });
    Check(!present, "an unset display setting emits no flag at all");
  }
}

void TestSetSettingsEmitFlags() {
  thps::Settings settings;
  settings.resolution = "1440p";
  settings.monitor = 2;
  settings.fullscreen = 1;
  settings.vsync = 0;
  const std::vector<std::string> argv = thps::RenderArgv(Paths(), settings);
  Check(Has(argv, "--resolution=1440p"), "resolution is rendered");
  Check(Has(argv, "--monitor=2"), "monitor is rendered");
  Check(Has(argv, "--fullscreen=true"), "fullscreen=true is rendered");
  // The one a naive `if (value)` gets wrong: false is a real choice and must
  // emit, because the game's own default is on.
  Check(Has(argv, "--vsync=false"), "vsync=false is rendered, not dropped as falsy");
  Check(!AnyEmptyValuedFlag(argv), "no empty-valued flag with everything set");
}

void TestMonitorZeroIsASetting() {
  // Display 0 is the primary display and a perfectly ordinary choice. Treating
  // 0 as "unset" is the same falsy-value bug as vsync=false, one type along.
  thps::Settings settings;
  settings.monitor = 0;
  const std::vector<std::string> argv = thps::RenderArgv(Paths(), settings);
  Check(Has(argv, "--monitor=0"), "monitor 0 is a choice, not an absence");
}

void TestInvalidSettingsAreDroppedNotPassed() {
  // settings.toml is user-editable text. A hand-typed value must degrade to the
  // default rather than reaching the game's command line.
  thps::Settings settings;
  settings.resolution = "1081p";
  settings.fullscreen = 7;
  const std::vector<std::string> argv = thps::RenderArgv(Paths(), settings);
  Check(!Has(argv, "--resolution=1081p"), "an unknown resolution never reaches argv");
  Check(!std::any_of(argv.begin(), argv.end(),
                     [](const std::string& a) { return a.rfind("--fullscreen", 0) == 0; }),
        "an out-of-range tri-state is dropped");
}

void TestRoundTrip() {
  thps::Settings settings;
  settings.resolution = "1080p";
  settings.monitor = 1;
  settings.fullscreen = 0;
  settings.vsync = 1;
  const thps::Settings back = thps::ParseSettings(thps::SerialiseSettings(settings));
  Check(back.resolution == "1080p", "resolution survives a round trip");
  Check(back.monitor == 1, "monitor survives a round trip");
  Check(back.fullscreen == 0, "fullscreen=false survives a round trip as false, not unset");
  Check(back.vsync == 1, "vsync survives a round trip");
}

void TestUnsetSurvivesRoundTripAsUnset() {
  // The distinction the whole tri-state exists for: absent must not come back
  // as false, or a fresh install would start emitting flags it never chose.
  const thps::Settings back = thps::ParseSettings(thps::SerialiseSettings(thps::Settings{}));
  Check(back.resolution.empty(), "unset resolution stays unset");
  Check(back.monitor == -1, "unset monitor stays unset");
  Check(back.fullscreen == -1, "unset fullscreen stays unset, not false");
  Check(back.vsync == -1, "unset vsync stays unset, not false");
}

void TestExplicitModeRendersAsAVideoMode() {
  // 1280x800 is the Steam Deck's panel and is NOT one of --resolution's named
  // presets, which are all 16:9. It has to reach the game as an explicit guest
  // video mode instead; rendering it as `--resolution=1280x800` would be
  // rejected by the runtime's own validation, and a launcher whose Play button
  // produces a flag the game refuses is worse than one that never offered it.
  thps::Settings settings;
  settings.resolution = "1280x800";
  const std::vector<std::string> argv = thps::RenderArgv(Paths(), settings);
  Check(Has(argv, "--window_width=1280"), "explicit mode renders a window width");
  Check(Has(argv, "--window_height=800"), "explicit mode renders a window height");
  Check(Has(argv, "--video_mode_width=1280"), "explicit mode renders a width");
  Check(Has(argv, "--video_mode_height=800"), "explicit mode renders a height");
  Check(Has(argv, "--fullscreen=false"),
        "a selected size becomes visible when fullscreen was unset");
  Check(!Has(argv, "--resolution=1280x800"),
        "an explicit mode is never rendered as a --resolution preset");
  Check(!AnyEmptyValuedFlag(argv), "no empty-valued flag for an explicit mode");

  // The named presets must not have started going down the new path.
  thps::Settings preset;
  preset.resolution = "720p";
  const std::vector<std::string> preset_argv = thps::RenderArgv(Paths(), preset);
  Check(Has(preset_argv, "--resolution=720p"), "a named preset still renders as one");
  Check(!Has(preset_argv, "--video_mode_width=1280"),
        "a named preset does not also emit a video mode");
  Check(Has(preset_argv, "--fullscreen=false"),
        "a named size also becomes visible when fullscreen was unset");
}

void TestExplicitFullscreenWinsOverWindowSizeDefault() {
  thps::Settings settings;
  settings.resolution = "1080p";
  settings.fullscreen = 1;
  const std::vector<std::string> argv = thps::RenderArgv(Paths(), settings);
  Check(Has(argv, "--resolution=1080p"), "the selected size is still passed");
  Check(Has(argv, "--fullscreen=true"), "explicit fullscreen wins");
  Check(!Has(argv, "--fullscreen=false"),
        "the implicit windowed flag is not emitted beside explicit fullscreen");
}

void TestExplicitModeSurvivesValidation() {
  // Sanitise() drops anything not in ResolutionChoices(), so a new choice that
  // was added to the list but not to the validator would be silently erased on
  // the way to argv - the failure would look like "the setting does not stick".
  thps::Settings settings;
  settings.resolution = "1280x800";
  Check(settings.Valid(), "1280x800 is a valid choice");
  settings.Sanitise();
  Check(settings.resolution == "1280x800", "1280x800 survives Sanitise");

  const thps::Settings back =
      thps::ParseSettings(thps::SerialiseSettings(settings));
  Check(back.resolution == "1280x800", "1280x800 survives a file round trip");

  // And a hand-typed near-miss still does not reach argv.
  thps::Settings typo;
  typo.resolution = "1280x801";
  Check(!typo.Valid(), "an unlisted WxH is not valid just because it parses");
  typo.Sanitise();
  Check(typo.resolution.empty(), "an unlisted WxH sanitises away");
}

void TestPerformanceIsOnUnlessTurnedOff() {
  // The asymmetry this file exists to police, applied deliberately once: every
  // other setting emits nothing when unset, and this one emits its flags. A
  // fresh install must be fast without anyone opening the settings screen.
  const std::vector<std::string> unset = thps::RenderArgv(Paths(), thps::Settings{});
  Check(Has(unset, "--gpu_cp_fastpath=true"),
        "unset performance still enables the fastpath");
  Check(Has(unset, "--primitive_processor_cache_min_indices=4096"),
        "unset performance still sets the primitive processor cache threshold");
  Check(Has(unset, "--gpu_native_residency=true"),
        "unset performance still enables native residency");
  Check(Has(unset, "--gpu_native_prefetch=true"),
        "unset performance still enables frame-ahead vertex prefetch");
  Check(Has(unset, "--gpu_sampler_set_reuse=true"),
        "unset performance still enables adjacent sampler reuse");
  Check(Has(unset, "--gpu_texture_request_reuse=true"),
        "unset performance still enables exact texture-request reuse");
  Check(Has(unset, "--gpu_texture_last_view_cache=true"),
        "unset performance still enables exact last-view reuse");
  Check(Has(unset, "--gpu_texture_descriptor_set_adjacent_reuse=true"),
        "unset performance still enables adjacent descriptor-set reuse");
  Check(Has(unset, "--timer_queue_blocking_wait=true"),
        "unset performance still blocks the idle timer queue");
  Check(Has(unset, "--guest_ring_wait_backoff=true"),
        "unset performance still backs off the guest empty-ring wait");
  Check(Has(unset, "--guest_ring_wait_event=true"),
        "unset performance still blocks on the guest ring producer");
  Check(Has(unset, "--guest_swap_wait=true"),
        "unset performance still blocks on guest swap completion");
  Check(Has(unset, "--guest_u8x4_unpack_native=true"),
        "unset performance still enables the exact u8x4 unpack");
  Check(Has(unset, "--guest_vertex_unpack_native=true"),
        "unset performance still enables the exact vertex-record unpack");
  Check(Has(unset, "--guest_vertex_unpack_simd=true"),
        "unset performance still enables the exact SIMD vertex-record unpack");

  thps::Settings on;
  on.performance = 1;
  Check(thps::RenderArgv(Paths(), on).size() == unset.size(),
        "explicitly on renders the same argv as unset");

  // Off must emit NOTHING, not `=false`: the runtime's own defaults are already
  // off, and emitting the negation would be a second way to say the same thing.
  thps::Settings off;
  off.performance = 0;
  const std::vector<std::string> argv = thps::RenderArgv(Paths(), off);
  for (const auto& [name, value] : thps::Settings::PerformanceFlags()) {
    (void)value;
    for (const std::string& arg : argv) {
      Check(arg.rfind("--" + name + "=", 0) != 0,
            ("off emits no flag for " + name).c_str());
    }
  }
  Check(!AnyEmptyValuedFlag(argv), "no empty-valued flag with performance off");
}

void TestPerformanceSurvivesARoundTrip() {
  thps::Settings off;
  off.performance = 0;
  Check(thps::ParseSettings(thps::SerialiseSettings(off)).performance == 0,
        "performance=off survives a round trip");
  // Unset must NOT be written, or it would freeze today's meaning of "unset"
  // into the file and stop tracking the default.
  Check(thps::SerialiseSettings(thps::Settings{}).find("performance") ==
            std::string::npos,
        "unset performance is not written to the file");
}

void TestUnknownKeysAreIgnored() {
  // A settings file written by a newer build must still load in an older one.
  const thps::Settings settings = thps::ParseSettings(
      "[display]\nresolution = \"720p\"\nray_tracing = true\n# a comment\n\n");
  Check(settings.resolution == "720p", "a known key still parses beside an unknown one");
}

}  // namespace

int main() {
  TestRequiredFlagsAlwaysPresent();
  TestDefaultSettingsEmitNoDisplayFlags();
  TestSetSettingsEmitFlags();
  TestMonitorZeroIsASetting();
  TestInvalidSettingsAreDroppedNotPassed();
  TestRoundTrip();
  TestUnsetSurvivesRoundTripAsUnset();
  TestPerformanceIsOnUnlessTurnedOff();
  TestPerformanceSurvivesARoundTrip();
  TestExplicitModeRendersAsAVideoMode();
  TestExplicitModeSurvivesValidation();
  TestExplicitFullscreenWinsOverWindowSizeDefault();
  TestUnknownKeysAreIgnored();

  if (g_failures) {
    std::fprintf(stderr, "\n%d assertion(s) failed\n", g_failures);
    return 1;
  }
  std::printf("settings: all assertions passed\n");
  return 0;
}
