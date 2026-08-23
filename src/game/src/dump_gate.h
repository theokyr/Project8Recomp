// The runtime dump gate.
//
// `thps_p8_identify` guards the launcher's first-run path: it refuses a wrong
// disc before 4.7 GB is copied. That covers exactly one way into the game. This
// covers the rest - `make play`, a bare `thps_p8 --game_data_root=...`, a
// `thps_p8_launch` passthrough, and a portable directory whose `game/` folder
// someone emptied, moved or half-copied.
//
// What it replaces is not "no check". The runtime already notices: it logs
//
//     [ERROR] Entrypoint XEX not found
//
// and then aborts inside its own teardown with `double free or corruption
// (!prev)`, exit 134, core dumped. The detection works; the *refusal* is what
// is missing, and a core dump is not failing closed - it is indistinguishable
// from a bug in the port, which is precisely the impression a first-time user
// would take away.
//
// Three properties this has to hold, in order of how easily they are lost:
//
//   1. **One table.** The accepted set is `common/supported_dumps.h`, shared
//      verbatim with `thps_p8_identify`. A second copy would drift and only one
//      of the two gates would ever be exercised.
//   2. **It runs before the Runtime exists.** OnConfigurePaths is the last hook
//      before construction, so a refusal here leaves nothing half-built to tear
//      down - which is the whole point, given that teardown is where the crash
//      lives.
//   3. **No override.** There is no `--skip_dump_check`. An unknown hash is a
//      refusal. The gate is the only remaining guarantee that the assets a user
//      supplies match the build compiled against them, now that the executable
//      ships prebuilt, so a flag that turns it off would be a flag that turns
//      the guarantee off.
//
// Cost is ~54 ms: an 8 MB read and one sha256, once, before the window opens.
// Measured against a boot that takes seconds. Not worth caching, and a cache
// would be a second thing that can be wrong about which dump is installed.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <rex/crypto/sha256.h>
#include <rex/logging.h>

#include "common/supported_dumps.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace dump_gate {

enum class Verdict {
  kOk,          // hash matched a row in the table
  kNoRoot,      // --game_data_root points at nothing
  kNoXex,       // the directory exists but has no default.xex
  kUnreadable,  // it is there and could not be read
  kWrongBuild,  // a default.xex of a supported size, but not a supported hash
  kUnknown,     // a default.xex that is neither
};

struct Result {
  Verdict verdict = Verdict::kOk;
  std::string_view release;  // set only on kOk
};

// Pure, so the decision can be tested without a game around it.
inline Result Evaluate(const std::filesystem::path& game_data_root) {
  std::error_code ec;
  if (game_data_root.empty() || !std::filesystem::is_directory(game_data_root, ec)) {
    return {Verdict::kNoRoot, {}};
  }
  const std::filesystem::path xex = game_data_root / "default.xex";
  if (!std::filesystem::is_regular_file(xex, ec)) {
    return {Verdict::kNoXex, {}};
  }

  std::ifstream in(xex, std::ios::binary);
  if (!in) {
    return {Verdict::kUnreadable, {}};
  }
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    return {Verdict::kUnreadable, {}};
  }
  const std::string digest = rex::crypto::sha256(std::string_view(bytes));

  for (const auto& row : thps::identify::kSupportedDumps) {
    if (row.sha256 == digest) {
      return {Verdict::kOk, row.release};
    }
  }
  // Same size as a supported release is weak evidence of "same game, built
  // differently" and it only changes the wording. `thps_p8_identify` draws the
  // same distinction the same way and for the same reason; when a XEX header
  // parser lands in either place, both should move to title_id together.
  for (const auto& row : thps::identify::kSupportedDumps) {
    if (bytes.size() == row.xex_size) {
      return {Verdict::kWrongBuild, {}};
    }
  }
  return {Verdict::kUnknown, {}};
}

inline const char* Explain(Verdict v) {
  switch (v) {
    case Verdict::kOk:
      return "";
    case Verdict::kNoRoot:
      return "No game data was found.\n"
             "This port ships no game content. It needs the files from your own copy of the\n"
             "game, and it was not told where they are.";
    case Verdict::kNoXex:
      return "That folder does not contain the game.\n"
             "It exists, but the game's own files are not in it. If a previous setup was\n"
             "interrupted, run the launcher again and let it finish.";
    case Verdict::kUnreadable:
      return "The game files could not be read.\n"
             "They may be incomplete, or this account may not have permission to read them.";
    case Verdict::kWrongBuild:
      return "Those game files are from a different release.\n"
             "This is the right game, but not the release this port was built from. Only the\n"
             "release listed by the launcher will work.";
    case Verdict::kUnknown:
      return "Those files are not the game this port plays.\n"
             "The folder contains a game, but not the one this port was built for.";
  }
  return "";
}

// Refuses by leaving, and by leaving the short way.
//
// `std::exit` would run static destructors, and this process is being stopped
// precisely because its teardown path is the one that aborts. `_exit` after an
// explicit flush is what the SIGTERM handler already does here and for the same
// reason: everything worth saying has been said, and nothing that remains is
// worth risking a core dump over a message the user has already read.
[[noreturn]] inline void Refuse(const Result& result,
                                const std::filesystem::path& game_data_root) {
  std::fprintf(stderr,
               "\n"
               "Tony Hawk's Project 8 cannot start.\n"
               "\n"
               "%s\n"
               "\n"
               "Looked in: %s\n"
               "\n"
               "This port contains no game content and will never download any. You supply\n"
               "the files from a copy you own; nothing else will do.\n"
               "\n",
               Explain(result.verdict),
               game_data_root.empty() ? "(nowhere - no game data folder was given)"
                                      : game_data_root.string().c_str());
  std::fflush(stderr);
  std::fflush(stdout);
#ifdef _WIN32
  ::_exit(3);
#else
  ::_exit(3);
#endif
}

// The whole gate, as one call from OnConfigurePaths.
inline void Enforce(const std::filesystem::path& game_data_root) {
  const Result result = Evaluate(game_data_root);
  if (result.verdict == Verdict::kOk) {
    REXLOG_INFO("dump gate: accepted {} at {}", std::string(result.release),
                game_data_root.string());
    return;
  }
  Refuse(result, game_data_root);
}

}  // namespace dump_gate
