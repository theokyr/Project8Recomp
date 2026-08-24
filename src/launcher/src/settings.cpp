#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace thps {
namespace {

std::string Trim(const std::string& s) {
  const size_t first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

// Strips the quotes a TOML string carries. The writer emits them; a human
// editing the file by hand may not, so both are accepted.
std::string Unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

// Tri-state, because "absent" and "false" are different answers and only one of
// them emits a flag.
int ParseTriBool(const std::string& raw) {
  const std::string v = Unquote(Trim(raw));
  if (v == "true") return 1;
  if (v == "false") return 0;
  return -1;
}

const char* TriBoolText(int v) { return v == 1 ? "true" : "false"; }

}  // namespace

const std::vector<std::string>& Settings::ResolutionChoices() {
  // "" first and unnamed here; the UI labels it "Game default".
  //
  // Everything except "1280x800" is a preset the game's --resolution flag
  // accepts. 1280x800 is not one of them, and cannot be: --resolution takes
  // named 16:9 presets and the Steam Deck's panel is 16:10. It is offered
  // anyway because it is the panel the port is most likely to be played on,
  // and it renders as an explicit guest video mode rather than a preset - see
  // RenderArgv.
  //
  // It is a MODE THE 2006 TITLE NEVER SHIPPED. The Xbox 360 offered 4:3 and
  // 16:9; nothing in the retail build was framed for 16:10, so HUD placement
  // and FMV framing are the things to look at before trusting it. That is why
  // it is selectable and not the default: 720p letterboxed on the Deck's panel
  // is the safe arm, and the choice between them is being made from mark
  // screenshots taken on the hardware rather than from reasoning.
  static const std::vector<std::string> choices = {
      "", "720p", "1080p", "1440p", "4k", "1280x800"};
  return choices;
}

bool Settings::IsExplicitMode(const std::string& value, int* width, int* height) {
  const size_t x = value.find('x');
  if (x == std::string::npos || x == 0 || x + 1 >= value.size()) return false;
  int w = 0;
  int h = 0;
  try {
    w = std::stoi(value.substr(0, x));
    h = std::stoi(value.substr(x + 1));
  } catch (...) {
    return false;
  }
  if (w <= 0 || h <= 0) return false;
  if (width) *width = w;
  if (height) *height = h;
  return true;
}

const std::vector<std::pair<std::string, std::string>>& Settings::PerformanceFlags() {
  // gpu_cp_fastpath slims the command-processor parse loop; the primitive
  // processor cache threshold stops re-converting small index buffers; native
  // residency makes vertex and index data resident by copying instead of
  // page-watching it; frame-ahead prefetch moves the predicted vertex upload
  // wave before the render pass; adjacent sampler reuse avoids redundant
  // same-submission Vulkan sampler lookups; exact texture-request reuse skips
  // an unchanged binding and image-usage walk; exact last-view reuse bypasses
  // the Vulkan view-key map for an immediately repeated request; adjacent
  // descriptor-set reuse skips an exact repeated stage allocation and write;
  // the timer queue blocks between deadlines instead of yield-spinning; the guest
  // empty-ring wait backs off
  // after a bounded poll interval or blocks on its exact producer event; the
  // render thread blocks on the actual swap-complete counter producer; the
  // title's hot u8x4 render-preparation unpack bypasses the full guest vector
  // register model; the title's full vertex-format record unpack does the same
  // for the fixed 252-byte-to-496-byte transform, and its x86 SIMD path keeps
  // the exact transform while vectorizing the packed decodes. All seventeen
  // default OFF in the runtime because they are project patches rather than
  // upstream behaviour - which is exactly why the launcher has to ask for them.
  static const std::vector<std::pair<std::string, std::string>> flags = {
      {"gpu_cp_fastpath", "true"},
      {"primitive_processor_cache_min_indices", "4096"},
      {"gpu_native_residency", "true"},
      {"gpu_native_prefetch", "true"},
      {"gpu_native_index", "true"},
      {"gpu_native_vertex", "true"},
      {"gpu_sampler_set_reuse", "true"},
      {"gpu_texture_request_reuse", "true"},
      {"gpu_texture_last_view_cache", "true"},
      {"gpu_texture_descriptor_set_adjacent_reuse", "true"},
      {"timer_queue_blocking_wait", "true"},
      {"guest_ring_wait_backoff", "true"},
      {"guest_ring_wait_event", "true"},
      {"guest_swap_wait", "true"},
      {"guest_u8x4_unpack_native", "true"},
      {"guest_vertex_unpack_native", "true"},
      {"guest_vertex_unpack_simd", "true"},
  };
  return flags;
}

bool Settings::Valid() const {
  const auto& choices = ResolutionChoices();
  if (std::find(choices.begin(), choices.end(), resolution) == choices.end()) return false;
  if (monitor < -1) return false;
  if (fullscreen < -1 || fullscreen > 1) return false;
  if (vsync < -1 || vsync > 1) return false;
  if (performance < -1 || performance > 1) return false;
  return true;
}

void Settings::Sanitise() {
  const auto& choices = ResolutionChoices();
  if (std::find(choices.begin(), choices.end(), resolution) == choices.end()) resolution.clear();
  if (monitor < -1) monitor = -1;
  if (fullscreen < -1 || fullscreen > 1) fullscreen = -1;
  if (vsync < -1 || vsync > 1) vsync = -1;
  if (performance < -1 || performance > 1) performance = -1;
}

std::vector<std::string> RenderArgv(const LaunchPaths& paths, const Settings& settings) {
  std::vector<std::string> argv;
  argv.push_back(paths.launcher_exe.string());
  argv.push_back("--game_data_root=" + paths.game_data_root.string());
  argv.push_back("--mnk_mode=true");
  argv.push_back("--gpu_plugin=xenos");
  if (!paths.breadcrumb.empty()) {
    argv.push_back("--launcher-breadcrumb=" + paths.breadcrumb.string());
  }

  // The omit-empty rule from here down. Every one of these is `if set, emit;
  // else emit nothing` - never `--flag=` with an empty value.
  Settings s = settings;
  s.Sanitise();

  if (!s.resolution.empty()) {
    int width = 0;
    int height = 0;
    if (Settings::IsExplicitMode(s.resolution, &width, &height)) {
      // The host window and guest video mode, set directly. --resolution only
      // understands its named presets, and passing it "1280x800" would be
      // rejected by the runtime's own validation rather than silently ignored.
      // Setting only the guest mode is not enough: this title keeps its own
      // render targets fixed, so the visible effect of this setting is the
      // host window size.
      argv.push_back("--window_width=" + std::to_string(width));
      argv.push_back("--window_height=" + std::to_string(height));
      argv.push_back("--video_mode_width=" + std::to_string(width));
      argv.push_back("--video_mode_height=" + std::to_string(height));
    } else {
      argv.push_back("--resolution=" + s.resolution);
    }
  }
  if (s.monitor >= 0) {
    argv.push_back("--monitor=" + std::to_string(s.monitor));
  }
  if (s.fullscreen >= 0) {
    argv.push_back(std::string("--fullscreen=") + TriBoolText(s.fullscreen));
  } else if (!s.resolution.empty()) {
    // The SDK defaults to borderless fullscreen at the desktop's dimensions,
    // where every requested window size is discarded. A user who chooses a
    // size - including by editing settings.toml - must see that choice take
    // effect. An explicit Fullscreen choice still wins.
    argv.push_back("--fullscreen=false");
  }
  if (s.vsync >= 0) {
    argv.push_back(std::string("--vsync=") + TriBoolText(s.vsync));
  }

  // The one setting whose UNSET state emits flags. See settings.h: the
  // runtime's defaults are off, and a player who never opens the settings
  // screen should still get the configuration this port was measured in.
  // Explicitly off emits nothing, which lands back on those runtime defaults.
  if (s.performance != 0) {
    for (const auto& [name, value] : Settings::PerformanceFlags()) {
      argv.push_back("--" + name + "=" + value);
    }
  }
  return argv;
}

std::string SerialiseSettings(const Settings& settings) {
  std::ostringstream out;
  out << "# Written by the Tony Hawk's Project 8 launcher.\n"
      << "# Delete this file to go back to the defaults.\n"
      << "#\n"
      << "# An absent or commented-out key means \"leave it to the game\", which is\n"
      << "# not the same as setting it to false.\n"
      << "[display]\n";
  // Absent, not empty. An empty value here would round-trip to an empty value
  // in the argv renderer, and the whole point of the omit-empty rule is that
  // those are different things.
  if (!settings.resolution.empty()) out << "resolution = \"" << settings.resolution << "\"\n";
  if (settings.monitor >= 0) out << "monitor = " << settings.monitor << "\n";
  if (settings.fullscreen >= 0) out << "fullscreen = " << TriBoolText(settings.fullscreen) << "\n";
  if (settings.vsync >= 0) out << "vsync = " << TriBoolText(settings.vsync) << "\n";
  if (settings.performance >= 0)
    out << "performance = " << TriBoolText(settings.performance) << "\n";
  return out.str();
}

Settings ParseSettings(const std::string& text) {
  Settings settings;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == '[') continue;
    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(trimmed.substr(0, eq));
    const std::string value = Trim(trimmed.substr(eq + 1));
    if (key == "resolution") {
      settings.resolution = Unquote(value);
    } else if (key == "monitor") {
      settings.monitor = int(std::strtol(Unquote(value).c_str(), nullptr, 10));
    } else if (key == "fullscreen") {
      settings.fullscreen = ParseTriBool(value);
    } else if (key == "vsync") {
      settings.vsync = ParseTriBool(value);
    } else if (key == "performance") {
      settings.performance = ParseTriBool(value);
    }
    // Unknown keys are skipped on purpose: a file written by a newer build must
    // still load in an older one rather than being rejected wholesale.
  }
  settings.Sanitise();
  return settings;
}

bool LoadSettings(const std::filesystem::path& file, Settings* out) {
  std::ifstream in(file);
  if (!in) return false;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  *out = ParseSettings(text);
  return true;
}

bool SaveSettings(const std::filesystem::path& file, const Settings& settings) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  // Write-then-rename, so a full disk or a kill mid-write leaves the previous
  // settings intact rather than a truncated file that parses to defaults.
  const std::filesystem::path temp = file.string() + ".new";
  {
    std::ofstream f(temp, std::ios::trunc);
    if (!f) return false;
    f << SerialiseSettings(settings);
    if (!f) return false;
  }
  std::filesystem::rename(temp, file, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
}

}  // namespace thps
