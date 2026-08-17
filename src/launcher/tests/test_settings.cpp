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
  TestUnknownKeysAreIgnored();

  if (g_failures) {
    std::fprintf(stderr, "\n%d assertion(s) failed\n", g_failures);
    return 1;
  }
  std::printf("settings: all assertions passed\n");
  return 0;
}
