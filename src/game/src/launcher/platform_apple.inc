// platform_apple.inc - included from platform.h on macOS.
//
// Process inspection through sysctl's KERN_PROC_ALL table, which carries the
// name, the parent and the run state in one pass without needing entitlements.
//
// No sweep, and this one IS a real limitation rather than a finished answer:
// macOS POSIX shm names live in a kernel namespace with no directory to list
// (there is no /dev/shm), so an orphaned segment cannot be discovered even in
// principle - only unlinked by a name something already knows. If the port is
// ever built here and the runtime turns out to leak, the fix has to be a
// launcher-side record of the name it handed out, not a scan.
//
// VERIFIED 2026-08-02: built as arm64 and used for native attract, Free Skate
// and career fixture launches. Preflight, SelfPath and the unsupported-sweep
// result all execute on every launcher run.

#pragma once

#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cstring>

namespace launcher::platform {

inline std::vector<ProcessInfo> FindProcesses(const std::string& name) {
  std::vector<ProcessInfo> found;
  if (name.empty()) return found;

  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
  std::size_t length = 0;
  if (::sysctl(mib, 4, nullptr, &length, nullptr, 0) != 0 || length == 0) return found;
  // The table can grow between sizing and reading; over-allocate a little and
  // accept a short read rather than looping forever.
  length += length / 8 + sizeof(kinfo_proc);
  std::vector<char> buffer(length);
  if (::sysctl(mib, 4, buffer.data(), &length, nullptr, 0) != 0) return found;

  const std::size_t count = length / sizeof(kinfo_proc);
  const auto* procs = reinterpret_cast<const kinfo_proc*>(buffer.data());
  const pid_t self = ::getpid();
  for (std::size_t i = 0; i < count; ++i) {
    const kinfo_proc& kp = procs[i];
    const pid_t pid = kp.kp_proc.p_pid;
    if (pid <= 0 || pid == self) continue;
    // p_comm is MAXCOMLEN (16) and truncated, like Linux's comm.
    if (std::strncmp(kp.kp_proc.p_comm, name.c_str(), MAXCOMLEN) != 0) continue;

    ProcessInfo info;
    info.pid = static_cast<int>(pid);
    info.ppid = static_cast<int>(kp.kp_eproc.e_ppid);
    switch (kp.kp_proc.p_stat) {
      case SZOMB: info.state = 'Z'; break;
      case SRUN:  info.state = 'R'; break;
      case SSLEEP: info.state = 'S'; break;
      case SSTOP: info.state = 'T'; break;
      default: info.state = '?'; break;
    }
    found.push_back(info);
  }
  return found;
}

inline SweepResult SweepOrphans(const std::string&, const std::string&, bool) {
  SweepResult result;
  result.ran = true;
  result.supported = false;  // no way to enumerate shm names - see the header
  return result;
}

inline std::string SelfPath(const char* argv0) {
  char buffer[4096];
  std::uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) == 0) return std::string(buffer);
  return argv0 ? std::string(argv0) : std::string();
}

}  // namespace launcher::platform
