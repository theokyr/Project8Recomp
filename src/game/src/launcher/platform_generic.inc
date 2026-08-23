// platform_generic.inc - included from platform.h on anything else.
//
// Keeps the launcher building on a platform nobody has ported it to yet. It
// still runs the game and still reports its exit status - only the two
// housekeeping duties degrade, and they degrade LOUDLY: FindProcesses returns
// nothing, so the preflight cannot refuse a second instance, and SweepOrphans
// reports supported=false so main.cpp says so rather than implying a clean
// sweep it never performed.
//
// Adding a platform means one more .inc and one more branch in platform.h.

#pragma once

#include <unistd.h>

namespace launcher::platform {

inline std::vector<ProcessInfo> FindProcesses(const std::string&) {
  return {};
}

inline SweepResult SweepOrphans(const std::string&, const std::string&, bool) {
  SweepResult result;
  result.ran = true;
  result.supported = false;
  return result;
}

inline std::string SelfPath(const char* argv0) {
  return argv0 ? std::string(argv0) : std::string();
}

}  // namespace launcher::platform
