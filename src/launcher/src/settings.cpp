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
  // "" first and unnamed here; the UI labels it "Game default". These are the
  // presets the game's --resolution flag accepts, and nothing else is offered
  // because nothing else is known to work.
  static const std::vector<std::string> choices = {"", "720p", "1080p", "1440p", "4k"};
  return choices;
}

bool Settings::Valid() const {
  const auto& choices = ResolutionChoices();
  if (std::find(choices.begin(), choices.end(), resolution) == choices.end()) return false;
  if (monitor < -1) return false;
  if (fullscreen < -1 || fullscreen > 1) return false;
  if (vsync < -1 || vsync > 1) return false;
  return true;
}

void Settings::Sanitise() {
  const auto& choices = ResolutionChoices();
  if (std::find(choices.begin(), choices.end(), resolution) == choices.end()) resolution.clear();
  if (monitor < -1) monitor = -1;
  if (fullscreen < -1 || fullscreen > 1) fullscreen = -1;
  if (vsync < -1 || vsync > 1) vsync = -1;
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
    argv.push_back("--resolution=" + s.resolution);
  }
  if (s.monitor >= 0) {
    argv.push_back("--monitor=" + std::to_string(s.monitor));
  }
  if (s.fullscreen >= 0) {
    argv.push_back(std::string("--fullscreen=") + TriBoolText(s.fullscreen));
  }
  if (s.vsync >= 0) {
    argv.push_back(std::string("--vsync=") + TriBoolText(s.vsync));
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
