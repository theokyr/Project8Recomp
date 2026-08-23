// Project8Recomp - the single player-facing portable entry point.
//
// It deliberately delegates to the existing launcher instead of starting the
// game binary directly. That keeps setup, settings, single-instance handling,
// crash cleanup and the supervisor on one path. The launcher's --play mode
// falls back to its setup UI when the install is incomplete.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "platform.h"

int main(int argc, char** argv) {
  bool show_gui = false;
  std::vector<std::string> forwarded;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--gui") == 0) {
      show_gui = true;
    } else if (std::strcmp(argv[i], "--play") == 0) {
      // Default behaviour already requests Play. Do not forward it when
      // --gui is also present: the explicit request for the UI must win.
      continue;
    } else if (std::strcmp(argv[i], "--help") == 0 ||
               std::strcmp(argv[i], "-h") == 0) {
      std::puts(
          "Project8Recomp [--gui] [launcher options]\n"
          "\n"
          "Without --gui, starts the game through the launcher and opens setup\n"
          "automatically when this portable install is not configured yet.\n"
          "With --gui, opens the launcher home screen.");
      return 0;
    } else {
      forwarded.emplace_back(argv[i]);
    }
  }

  const auto root = thps::platform::SelfDir();
  std::vector<std::string> command = {
      thps::platform::SiblingExe(root, "thps_p8_gui").string()};
  if (!show_gui) command.emplace_back("--play");
  command.insert(command.end(), forwarded.begin(), forwarded.end());
  thps::platform::ExecReplace(command);
}
