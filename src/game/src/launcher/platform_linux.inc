// platform_linux.inc - included from platform.h on Linux.
//
// Process inspection through /proc, and the only platform where guest-memory
// segments actually leak and can be reclaimed: POSIX shm objects are files in
// /dev/shm that outlive their process unless something calls shm_unlink.
//
// Safety for the sweep is blunt on purpose: if any non-zombie process with the
// game's name is alive, sweep nothing. Only that program creates these
// segments, so with no instance running every one is unowned by definition.
// Parsing /proc/*/maps to be cleverer would risk unlinking a segment out from
// under a live run - turning the janitor into the very SIGBUS it prevents.

#pragma once

#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace launcher::platform {
namespace detail {

inline constexpr const char* kShmDir = "/dev/shm";

// The kernel truncates comm to 15 characters, so compare truncated to
// truncated rather than against a full name that could never match.
inline std::string TruncateComm(const std::string& name) {
  return name.size() > 15 ? name.substr(0, 15) : name;
}

}  // namespace detail

inline std::vector<ProcessInfo> FindProcesses(const std::string& name) {
  std::vector<ProcessInfo> found;
  if (name.empty()) return found;
  const std::string want = detail::TruncateComm(name);
  DIR* proc = ::opendir("/proc");
  if (!proc) return found;
  const pid_t self = ::getpid();
  while (dirent* entry = ::readdir(proc)) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
    const int pid = std::atoi(entry->d_name);
    if (pid <= 0 || pid == static_cast<int>(self)) continue;
    std::ifstream comm_in(std::string("/proc/") + entry->d_name + "/comm");
    std::string comm;
    if (!std::getline(comm_in, comm)) continue;  // vanished, or not ours to read
    if (comm != want) continue;

    ProcessInfo info;
    info.pid = pid;
    // stat's comm field is parenthesised and may itself contain spaces or
    // brackets, so parse from the LAST ')' rather than splitting from the left.
    std::ifstream stat_in(std::string("/proc/") + entry->d_name + "/stat");
    std::string stat;
    if (std::getline(stat_in, stat)) {
      const std::size_t close = stat.rfind(')');
      if (close != std::string::npos && close + 2 < stat.size()) {
        char state = '?';
        int ppid = 0;
        if (std::sscanf(stat.c_str() + close + 2, "%c %d", &state, &ppid) == 2) {
          info.state = state;
          info.ppid = ppid;
        }
      }
    }
    found.push_back(info);
  }
  ::closedir(proc);
  return found;
}

inline SweepResult SweepOrphans(const std::string& process_name,
                                const std::string& prefix, bool dry_run) {
  SweepResult result;
  if (prefix.empty()) return result;

  for (const ProcessInfo& proc : FindProcesses(process_name)) {
    if (proc.IsZombie()) continue;  // has released its address space already
    result.declined_because =
        "'" + detail::TruncateComm(process_name) + "' is still running (pid " +
        std::to_string(proc.pid) + "); an orphan cannot be told from a segment in use";
    return result;
  }
  result.ran = true;

  DIR* dir = ::opendir(detail::kShmDir);
  if (!dir) return result;
  std::vector<std::string> names;
  while (dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.size() < prefix.size()) continue;
    if (name.compare(0, prefix.size(), prefix) != 0) continue;
    names.push_back(name);
  }
  ::closedir(dir);

  const uid_t self_uid = ::geteuid();
  for (const std::string& name : names) {
    const std::string path = std::string(detail::kShmDir) + "/" + name;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) continue;
    if (st.st_uid != self_uid) {
      ++result.skipped;
      continue;
    }
    // st_size is always the full sparse extent and says nothing about what is
    // being freed; blocks is what /dev/shm is really holding.
    const std::uint64_t bytes = static_cast<std::uint64_t>(st.st_blocks) * 512u;
    if (dry_run) {
      result.bytes += bytes;
      ++result.removed;  // "would remove"
      continue;
    }
    if (::shm_unlink(("/" + name).c_str()) == 0) {
      result.bytes += bytes;
      ++result.removed;
    }
  }
  return result;
}

inline std::string SelfPath(const char* argv0) {
  char buffer[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (n > 0) {
    buffer[n] = '\0';
    return std::string(buffer);
  }
  return argv0 ? std::string(argv0) : std::string();
}

}  // namespace launcher::platform
