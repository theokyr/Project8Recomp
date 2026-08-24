// thps_p8_launch - the port's launcher.
//
//   thps_p8_launch [launcher options] [--] [game arguments...]
//
// Runs the game as a CHILD process, waits for it however it ends, and reclaims
// what its own teardown could not. Everything it does not recognise is passed
// straight through, so it is a drop-in prefix for any existing command:
//
//   thps_p8_launch --game_data_root=/path --monitor=2 --gpu_plugin=xenos
//   thps_p8_launch --sweep-only            # reclaim and exit, run nothing
//   thps_p8_launch --check                 # report stale state, change nothing
//
// Launcher options are prefixed `--launcher-` where they could be confused with
// a game cvar, and everything after a bare `--` is passed through untouched, so
// a future game flag can never be shadowed by this program:
//
//   --sweep-only            reclaim orphaned segments and exit
//   --check                 report stale processes and segments, change nothing
//   --no-sweep              do not reclaim after the child exits
//   --force                 start even if an instance is already running
//   --json                  with --check only: the same facts on stdout as JSON
//   --launcher-breadcrumb=P record how the run ended to P, for the GUI
//   --launcher-game=PATH    game binary (default: `thps_p8` beside this one)
//   --launcher-help         this text
//
// ------------------------------------------------------------------- why
//
// Cleanup belongs in a parent process, not in the game. The runtime frees its
// guest-memory segment on a clean teardown, but the teardown mostly does not
// run - a hard exit on window close, an _exit(0) in the signal handler, and
// SIGKILL or a crash skipping everything. No in-process handler survives
// SIGKILL; a parent does. Measured before this existed: 35 orphans holding
// 13 GiB of a 16 GiB /dev/shm, after which the next launch died on SIGBUS.
//
// Cleanup runs on the child's DESTRUCTION, not on the next launch. The startup
// preflight is the backstop for the one case that cannot cover: this launcher
// being killed alongside its game, leaving nobody to reap. It reports what it
// finds - orphaned games whose launcher is gone, launchers stalled with no
// child, zombies nobody reaped, leftover segments - and refuses to start on top
// of a live instance rather than black-screening against it.
//
// This is also the seam the shipping launcher grows from: first-run dump path
// and extraction, then settings. Deliberately CLI-only for now - the
// pass-through and process-lifetime behaviour is what everything else sits on,
// so it is worth having correct and boring first.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "platform.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

#if defined(_WIN32)
constexpr const char* kDefaultGameName = "thps_p8.exe";
constexpr const char* kLauncherName = "thps_p8_launch.exe";
constexpr char kPathSep = '\\';
#else
constexpr const char* kDefaultGameName = "thps_p8";
constexpr const char* kLauncherName = "thps_p8_launch";
constexpr char kPathSep = '/';
#endif

// The shm names the runtime creates. Policy lives here; platform.h holds only
// the mechanism.
constexpr const char* kShmPrefix = "xenia_memory_";

struct Options {
  bool sweep_only = false;
  bool check_only = false;
  bool sweep_after = true;
  bool force = false;
  bool help = false;
  bool json = false;
  std::string breadcrumb;
  std::string game;
  std::vector<std::string> child_args;
};

std::string DirectoryOf(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string BaseNameOf(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

Options ParseArgs(int argc, char** argv) {
  Options options;
  bool passthrough = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    // Everything after a bare `--` belongs to the game, whatever it looks like.
    if (passthrough) {
      options.child_args.push_back(arg);
      continue;
    }
    if (arg == "--") {
      passthrough = true;
    } else if (arg == "--sweep-only") {
      options.sweep_only = true;
    } else if (arg == "--check") {
      options.check_only = true;
    } else if (arg == "--no-sweep") {
      options.sweep_after = false;
    } else if (arg == "--force") {
      options.force = true;
    } else if (arg == "--json") {
      options.json = true;
    } else if (arg.rfind("--launcher-breadcrumb=", 0) == 0) {
      options.breadcrumb = arg.substr(std::strlen("--launcher-breadcrumb="));
    } else if (arg == "--launcher-help") {
      options.help = true;
    } else if (arg.rfind("--launcher-game=", 0) == 0) {
      options.game = arg.substr(std::strlen("--launcher-game="));
    } else {
      options.child_args.push_back(arg);
    }
  }
  return options;
}

void PrintHelp() {
  std::printf(
      "thps_p8_launch - run the port and clean up after it.\n\n"
      "  thps_p8_launch [launcher options] [--] [game arguments...]\n\n"
      "Launcher options:\n"
      "  --sweep-only          reclaim orphaned guest-memory segments and exit\n"
      "  --check               report stale processes and segments, change nothing\n"
      "  --no-sweep            do not reclaim after the child exits\n"
      "  --force               start even if an instance is already running\n"
      "  --json                with --check only: the same facts on stdout as JSON\n"
      "  --launcher-breadcrumb=PATH  record how the run ended, for a GUI to read\n"
      "  --launcher-game=PATH  game binary (default: %s beside this one)\n"
      "  --launcher-help       this text\n\n"
      "Every other argument is passed through to the game unchanged; so is\n"
      "everything after a bare `--`.\n",
      kDefaultGameName);
}

void ReportSweep(const launcher::platform::SweepResult& result) {
  if (!result.supported) return;  // nothing to reclaim on this OS by design
  if (!result.ran) {
    std::fprintf(stderr, "thps_p8_launch: sweeping nothing - %s\n",
                 result.declined_because.c_str());
    return;
  }
  if (result.removed > 0) {
    std::fprintf(stderr,
                 "thps_p8_launch: reclaimed %zu orphaned segment(s), %.2f GiB\n",
                 result.removed,
                 static_cast<double>(result.bytes) / (1024.0 * 1024.0 * 1024.0));
  }
  if (result.skipped > 0) {
    std::fprintf(stderr, "thps_p8_launch: skipped %zu segment(s) owned by another user\n",
                 result.skipped);
  }
}

// Reports stale state and answers one question: is it safe to start?
//
// The cases worth naming separately, because they mean different things to
// whoever is reading:
//
//   live game        two instances fight over the shader cache and GPU and both
//                    black-screen, so this is a refusal, not a warning.
//   orphaned game    running with init as its parent - its launcher died, so
//                    nothing is going to clean up after it.
//   zombie game      already released its memory, waiting to be reaped. Holds
//                    no segment and blocks nothing.
//   stalled launcher a launcher with no game of its own, usually left by a
//                    child that was killed out from under it.
// `reporting` suppresses the decision - --check describes what it found and
// never speaks about starting or refusing, because it does neither.
bool Preflight(const std::string& game_name, bool force, bool reporting = false) {
  const auto games = launcher::platform::FindProcesses(game_name);
  const auto launchers = launcher::platform::FindProcesses(kLauncherName);

  std::size_t live = 0;
  for (const auto& proc : games) {
    if (proc.IsZombie()) {
      std::fprintf(stderr,
                   "thps_p8_launch: pid %d is a zombie %s - nobody reaped it. It holds\n"
                   "                no memory and blocks nothing.\n",
                   proc.pid, game_name.c_str());
      continue;
    }
    ++live;
    if (proc.IsOrphaned()) {
      std::fprintf(stderr,
                   "thps_p8_launch: pid %d is a running %s with no launcher (adopted by\n"
                   "                init) - nothing will clean up after it. 'make stop'.\n",
                   proc.pid, game_name.c_str());
    } else {
      std::fprintf(stderr, "thps_p8_launch: pid %d is already running %s\n", proc.pid,
                   game_name.c_str());
    }
  }

  // A launcher with no game is one whose child is gone. Ours is not in this
  // list (FindProcesses excludes self), so any hit is a leftover.
  if (!games.empty() || !launchers.empty()) {
    for (const auto& proc : launchers) {
      if (proc.IsZombie()) continue;
      bool has_child = false;
      for (const auto& game : games) {
        if (game.ppid == proc.pid) {
          has_child = true;
          break;
        }
      }
      if (!has_child) {
        std::fprintf(stderr,
                     "thps_p8_launch: pid %d is a launcher with no game - stalled, and\n"
                     "                safe to kill.\n",
                     proc.pid);
      }
    }
  }

  if (live == 0) return true;
  if (reporting) return false;
  if (force) {
    std::fprintf(stderr,
                 "thps_p8_launch: --force given; starting anyway. Two instances fight\n"
                 "                over the shader cache and GPU - expect a black screen.\n");
    return true;
  }
  std::fprintf(stderr,
               "thps_p8_launch: refusing to start on top of a live instance.\n"
               "                Run 'make stop', or pass --force.\n");
  return false;
}

#if defined(_WIN32)

// CreateProcess takes one command line, so each argument has to be re-quoted to
// survive the runtime's own parsing on the way back out.
std::string QuoteArgument(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
  std::string out = "\"";
  for (std::size_t i = 0; i < arg.size(); ++i) {
    std::size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++backslashes;
      ++i;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, '\\');
      break;
    }
    if (arg[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');
    } else {
      out.append(backslashes, '\\');
    }
    out.push_back(arg[i]);
  }
  out.push_back('"');
  return out;
}

int RunChild(const std::string& game, const std::vector<std::string>& args) {
  std::string command = QuoteArgument(game);
  for (const std::string& arg : args) {
    command += ' ';
    command += QuoteArgument(arg);
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  if (!CreateProcessA(game.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, 0,
                      nullptr, nullptr, &startup, &process)) {
    std::fprintf(stderr, "thps_p8_launch: cannot start %s (error %lu)\n", game.c_str(),
                 GetLastError());
    return 127;
  }
  // Console Ctrl+C reaches the whole group, so the child already gets it; we
  // only need to outlive it.
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
}

#else  // POSIX

volatile sig_atomic_t g_child = 0;

// Forward and then WAIT. The game's own handler is what flushes an in-progress
// input recording, so killing it out from under itself would cost the take.
extern "C" void ForwardSignal(int signo) {
  if (g_child > 0) {
    ::kill(g_child, signo);
  }
}

int RunChild(const std::string& game, const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(game.c_str()));
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    std::perror("thps_p8_launch: fork");
    return 127;
  }
  if (pid == 0) {
#if defined(__APPLE__)
    // The portable Mac archive carries its Vulkan loader and MoltenVK beside
    // the game. A GUI launch does not inherit Homebrew's shell environment, so
    // point the child at those packaged files before dyld and the loader run.
    const std::string game_dir = DirectoryOf(game);
    std::string library_path = game_dir;
    if (const char* inherited = std::getenv("DYLD_LIBRARY_PATH");
        inherited && *inherited) {
      library_path += ":";
      library_path += inherited;
    }
    const std::string driver_manifest = game_dir + "/MoltenVK_icd.json";
    ::setenv("DYLD_LIBRARY_PATH", library_path.c_str(), 1);
    ::setenv("VK_ICD_FILENAMES", driver_manifest.c_str(), 1);
#endif
    ::execv(game.c_str(), argv.data());
    std::fprintf(stderr, "thps_p8_launch: cannot exec %s: %s\n", game.c_str(),
                 std::strerror(errno));
    ::_exit(127);
  }

  g_child = pid;
  // A terminal delivers Ctrl-C to the whole foreground group, so the child gets
  // it directly too; forwarding covers being signalled on our own.
  std::signal(SIGINT, ForwardSignal);
  std::signal(SIGTERM, ForwardSignal);
  std::signal(SIGHUP, ForwardSignal);

  int status = 0;
  while (true) {
    const pid_t done = ::waitpid(pid, &status, 0);
    if (done == pid) break;
    if (done < 0 && errno == EINTR) continue;  // a forwarded signal interrupted us
    if (done < 0) {
      std::perror("thps_p8_launch: waitpid");
      return 127;
    }
  }
  g_child = 0;

  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    const int signo = WTERMSIG(status);
    std::fprintf(stderr, "thps_p8_launch: child terminated by signal %d (%s)\n", signo,
                 ::strsignal(signo));
    return 128 + signo;  // the shell convention, so `make` still sees a failure
  }
  return 0;
}

#endif

// --- machine-readable output ------------------------------------------------
//
// stdout only, and only under --check. Everything the text path prints keeps
// going to stderr byte-for-byte, so adding --json cannot change what a human or
// an existing script sees; the two streams carry the same facts in two shapes.
// Keep one stable envelope for every machine-readable disc-check result rather
// than inventing a second shape for this entry point.

std::string JsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (const char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// The four cases Preflight already names in prose, materialised so a consumer
// does not have to re-derive them from ppid and state.
const char* ClassifyGame(const launcher::platform::ProcessInfo& p) {
  if (p.IsZombie()) return "zombie";
  return p.IsOrphaned() ? "orphaned" : "live";
}

void EmitCheckJson(const std::string& game_path, const std::string& game_name,
                   const launcher::platform::SweepResult& sweep,
                   const std::vector<std::string>& child_args) {
  const auto games = launcher::platform::FindProcesses(game_name);
  const auto launchers = launcher::platform::FindProcesses(kLauncherName);

  std::size_t live = 0;
  for (const auto& proc : games) {
    if (!proc.IsZombie()) ++live;
  }

  std::printf("{\n");
  std::printf("  \"tool\": \"thps_p8_launch\",\n");
  std::printf("  \"command\": \"check\",\n");
  std::printf("  \"schema_version\": 1,\n");
  std::printf("  \"clear\": %s,\n", (live == 0 && !(sweep.ran && sweep.removed > 0)) ? "true" : "false");

  // std::filesystem rather than access(2): this reporting path is shared by
  // every platform, and X_OK/F_OK do not exist on Windows. "Executable" has no
  // direct equivalent there either - the OS decides by extension, not by a
  // permission bit - so the closest honest answer is "it is a regular file".
  std::error_code ec;
  const bool exists = std::filesystem::exists(game_path, ec);
#if defined(_WIN32)
  const bool executable = exists && std::filesystem::is_regular_file(game_path, ec);
#else
  const bool executable = ::access(game_path.c_str(), X_OK) == 0;
#endif
  std::printf("  \"game\": {\n");
  std::printf("    \"path\": \"%s\",\n", JsonEscape(game_path).c_str());
  std::printf("    \"name\": \"%s\",\n", JsonEscape(game_name).c_str());
  std::printf("    \"exists\": %s,\n", exists ? "true" : "false");
  std::printf("    \"executable\": %s\n", executable ? "true" : "false");
  std::printf("  },\n");

  std::printf("  \"processes\": {\n    \"game\": [");
  for (std::size_t i = 0; i < games.size(); ++i) {
    const auto& p = games[i];
    std::printf("%s\n      {\"pid\": %d, \"ppid\": %d, \"state\": \"%c\", \"zombie\": %s, "
                "\"orphaned\": %s, \"classification\": \"%s\"}",
                i ? "," : "", p.pid, p.ppid, p.state, p.IsZombie() ? "true" : "false",
                p.IsOrphaned() ? "true" : "false", ClassifyGame(p));
  }
  std::printf("%s],\n    \"launcher\": [", games.empty() ? "" : "\n    ");
  bool first = true;
  for (const auto& p : launchers) {
    if (p.IsZombie()) continue;
    // A launcher with no game is one whose child is gone: a leftover, not a
    // peer. Ours is never in this list - FindProcesses excludes self.
    bool has_child = false;
    for (const auto& g : games) {
      if (g.ppid == p.pid) has_child = true;
    }
    std::printf("%s\n      {\"pid\": %d, \"ppid\": %d, \"state\": \"%c\", \"zombie\": false, "
                "\"orphaned\": %s, \"has_child\": %s, \"classification\": \"%s\"}",
                first ? "" : ",", p.pid, p.ppid, p.state, p.IsOrphaned() ? "true" : "false",
                has_child ? "true" : "false", has_child ? "live" : "stalled");
    first = false;
  }
  std::printf("%s]\n  },\n", first ? "" : "\n    ");

  std::printf("  \"live_instances\": %zu,\n", live);
  std::printf("  \"sweep\": {\n");
  std::printf("    \"supported\": %s,\n", sweep.supported ? "true" : "false");
  std::printf("    \"dry_run\": true,\n");
  std::printf("    \"ran\": %s,\n", sweep.ran ? "true" : "false");
  std::printf("    \"removed\": %zu,\n", sweep.removed);
  std::printf("    \"bytes\": %llu,\n", (unsigned long long)sweep.bytes);
  std::printf("    \"skipped\": %zu,\n", sweep.skipped);
  std::printf("    \"declined_because\": \"%s\"\n", JsonEscape(sweep.declined_because).c_str());
  std::printf("  },\n");

  // stat only, never a verdict. The launcher does not form opinions about
  // passthrough args - the refusal lives in the game.
  std::string data_root;
  for (const std::string& arg : child_args) {
    if (arg.rfind("--game_data_root=", 0) == 0) {
      data_root = arg.substr(std::strlen("--game_data_root="));
    }
  }
  std::printf("  \"data_root\": {\n");
  std::printf("    \"given\": %s", data_root.empty() ? "false" : "true");
  if (!data_root.empty()) {
    std::error_code root_ec;
    const bool ok = std::filesystem::exists(data_root, root_ec);
    const bool is_dir = ok && std::filesystem::is_directory(data_root, root_ec);
    std::printf(",\n    \"path\": \"%s\",\n", JsonEscape(data_root).c_str());
    std::printf("    \"exists\": %s,\n", ok ? "true" : "false");
    std::printf("    \"is_directory\": %s\n", is_dir ? "true" : "false");
  } else {
    std::printf("\n");
  }
  std::printf("  }\n}\n");
}

// Closes the "the window vanished and nothing happened" hole: if the GUI execs
// this launcher and the game dies before drawing anything, the only evidence is
// an exit status nobody saw. Written after the child is reaped, so it records
// what actually happened rather than what was attempted.
void WriteBreadcrumb(const std::string& path, const std::string& game_path, int status,
                     const std::vector<std::string>& child_args) {
  if (path.empty()) return;
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    std::fprintf(stderr, "thps_p8_launch: cannot write breadcrumb %s: %s\n", path.c_str(),
                 std::strerror(errno));
    return;
  }
  std::fprintf(f, "{\n  \"tool\": \"thps_p8_launch\",\n  \"schema_version\": 1,\n");
  std::fprintf(f, "  \"game\": \"%s\",\n", JsonEscape(game_path).c_str());
  std::fprintf(f, "  \"exit_status\": %d,\n", status);
  // 128+n is the shell convention RunChild already returns for a signalled
  // child, so a consumer can tell a crash from a clean non-zero exit.
  std::fprintf(f, "  \"signalled\": %s,\n", status > 128 ? "true" : "false");
  std::fprintf(f, "  \"args\": [");
  for (std::size_t i = 0; i < child_args.size(); ++i) {
    std::fprintf(f, "%s\"%s\"", i ? ", " : "", JsonEscape(child_args[i]).c_str());
  }
  std::fprintf(f, "]\n}\n");
  std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = ParseArgs(argc, argv);
  if (options.help) {
    PrintHelp();
    return 0;
  }

  const std::string self = launcher::platform::SelfPath(argc > 0 ? argv[0] : nullptr);
  const std::string game =
      options.game.empty() ? DirectoryOf(self) + kPathSep + kDefaultGameName : options.game;
  // The game's own file name, so a renamed or relocated build still guards
  // itself correctly rather than looking for a hardcoded one.
  const std::string game_name = BaseNameOf(game);

  if (options.check_only) {
    // Reports only: no decision printed, and a dry-run sweep - a mode called
    // --check must not delete anything. Exit 1 when something is stale, so a
    // script can gate on it.
    const bool clear = Preflight(game_name, /*force=*/false, /*reporting=*/true);
    const auto result =
        launcher::platform::SweepOrphans(game_name, kShmPrefix, /*dry_run=*/true);
    if (!result.supported) {
      std::fprintf(stderr, "thps_p8_launch: no reclaimable segments on this platform\n");
    } else if (!result.ran) {
      std::fprintf(stderr, "thps_p8_launch: segments present, but a live instance means\n"
                           "                none can be shown to be orphaned\n");
    } else if (result.removed > 0) {
      std::fprintf(stderr, "thps_p8_launch: %zu orphaned segment(s) holding %.2f GiB\n"
                           "                would be reclaimed (--sweep-only)\n",
                   result.removed,
                   static_cast<double>(result.bytes) / (1024.0 * 1024.0 * 1024.0));
    } else {
      std::fprintf(stderr, "thps_p8_launch: clean - no stale processes or segments\n");
    }
    // `clear` only answers "is a live instance running" - Preflight with
    // reporting=true returns live == 0. Returning it alone meant the case this
    // mode exists for, orphaned segments with nothing alive holding them, was
    // reported on stderr and then exited 0, so any script gating on --check
    // saw a clean machine while /dev/shm filled up. Both conditions count as
    // stale.
    const bool orphans = result.ran && result.removed > 0;
    if (options.json) {
      EmitCheckJson(game, game_name, result, options.child_args);
    }
    return (clear && !orphans) ? 0 : 1;
  }

  if (options.json) {
    std::fprintf(stderr, "thps_p8_launch: --json is only meaningful with --check\n");
    return 2;
  }

  if (options.sweep_only) {
    ReportSweep(launcher::platform::SweepOrphans(game_name, kShmPrefix));
    return 0;
  }

  // Backstop for the one case the post-exit sweep cannot cover: this launcher
  // killed alongside its game, so nobody was left to reap. Usually silent.
  if (!Preflight(game_name, options.force)) {
    return 1;
  }
  if (options.sweep_after) {
    ReportSweep(launcher::platform::SweepOrphans(game_name, kShmPrefix));
  }

  const int status = RunChild(game, options.child_args);
  WriteBreadcrumb(options.breadcrumb, game, status, options.child_args);

  // The child is gone, so anything left under its name is debris by definition.
  // Done even when the child failed - a crash is exactly the case that leaks -
  // but never allowed to change the status make sees.
  if (options.sweep_after) {
    ReportSweep(launcher::platform::SweepOrphans(game_name, kShmPrefix));
  }
  return status;
}
