// platform.h - the launcher's OS-specific surface, and nothing else.
//
// Three operations, one implementation per platform in the .inc files beside
// this one. Everything in main.cpp is written against this interface, so the
// policy ("refuse to start if the game is already running") stays in one place
// and only the mechanics are per-OS.
//
//   FindProcesses  - live processes with a given executable name
//   SweepOrphans   - reclaim guest-memory segments no live process can be using
//   SelfPath       - this executable's own path, to find the game beside it
//
// ---------------------------------------------------------- portability note
//
// `SweepOrphans` is the one that genuinely differs, and not because of effort:
//
//   Linux   POSIX shm objects are files in /dev/shm. They outlive the process
//           unless something calls shm_unlink, so they can and do leak, and
//           they can be enumerated and removed. Real work to do here.
//   Windows A file-mapping object is refcounted by the kernel and released
//           with its last handle however the process died. Nothing leaks and
//           nothing can be enumerated. A no-op, correctly.
//   macOS   POSIX shm names live in a kernel namespace with no directory to
//           list, so orphans cannot be discovered even in principle. A no-op,
//           and flagged as a real limitation rather than a finished job.
//
// So "portable" here means: process inspection works everywhere, and the sweep
// does the right thing everywhere - which on two of the three is nothing.
//
// TESTED: Linux and macOS. The Apple path was built natively on arm64 and used
// for the attract, Free Skate and career fixture launches on 2026-08-02.
// Windows remains written against documented APIs and compile-guarded, but has
// not been built or run in this workspace.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace launcher::platform {

// One live process, as the preflight needs to see it.
struct ProcessInfo {
  int pid = 0;
  int ppid = 0;
  // Normalised across platforms: 'Z' zombie, 'R' runnable/running, 'S'
  // sleeping, 'T' stopped, '?' unknown. Only the zombie test is load-bearing.
  char state = '?';

  // A zombie has already released its address space, so it cannot be holding a
  // guest-memory segment - it is only waiting for a parent that has not reaped
  // it. Treating one as "an instance is running" would block the sweep for
  // exactly as long as some parent failed to reap, which is the case that most
  // needs sweeping.
  bool IsZombie() const { return state == 'Z'; }

  // Its parent is gone and init/launchd adopted it: a game still running with
  // no launcher watching, so nothing will clean up after it.
  bool IsOrphaned() const { return ppid == 1; }
};

struct SweepResult {
  bool ran = false;            // false when a live instance made it unsafe
  std::size_t removed = 0;
  std::uint64_t bytes = 0;     // committed, not apparent
  std::size_t skipped = 0;     // not owned by this user
  bool supported = true;       // false where the OS has nothing to reclaim
  std::string declined_because;
};

// Live processes whose executable name matches, excluding this one. `name` is
// the bare file name - "thps_p8" or "thps_p8.exe"; implementations match the
// way their OS reports it (Linux truncates to 15 chars, Windows is
// case-insensitive and keeps the extension).
std::vector<ProcessInfo> FindProcesses(const std::string& name);

// Reclaim orphaned guest-memory segments. `process_name` is the game's, used to
// refuse when an instance is alive; `prefix` is the shm name prefix.
//
// With `dry_run`, counts and measures exactly as usual but unlinks nothing - so
// a reporting mode can say what it WOULD reclaim without being a destructive
// operation wearing a read-only name.
SweepResult SweepOrphans(const std::string& process_name, const std::string& prefix,
                         bool dry_run = false);

// This executable's path, falling back to argv[0] where the OS offers nothing.
std::string SelfPath(const char* argv0);

}  // namespace launcher::platform

#if defined(_WIN32)
#include "platform_windows.inc"
#elif defined(__linux__)
#include "platform_linux.inc"
#elif defined(__APPLE__)
#include "platform_apple.inc"
#else
#include "platform_generic.inc"
#endif
