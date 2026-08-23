// platform_windows.inc - included from platform.h on Windows.
//
// Process inspection through the Toolhelp snapshot API. No sweep: a Windows
// file-mapping object is refcounted by the kernel and released with its last
// handle no matter how the process died - killed, crashed, or hard-exited - so
// there is nothing to leak and nothing to enumerate. That is a finished answer,
// not a stub.
//
// Windows has no zombie state either: a process object lingers only while
// somebody holds a handle to it, and it does not appear in the snapshot. So
// IsZombie() is always false here, which is correct rather than unimplemented.
//
// UNVERIFIED: written against the documented APIs, compiled and run nowhere.

#pragma once

#include <windows.h>
#include <tlhelp32.h>

#include <cctype>

namespace launcher::platform {
namespace detail {

inline bool EqualsNoCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace detail

inline std::vector<ProcessInfo> FindProcesses(const std::string& name) {
  std::vector<ProcessInfo> found;
  if (name.empty()) return found;
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return found;

  const DWORD self = GetCurrentProcessId();
  PROCESSENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (Process32First(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == self) continue;
      if (!detail::EqualsNoCase(entry.szExeFile, name)) continue;
      ProcessInfo info;
      info.pid = static_cast<int>(entry.th32ProcessID);
      info.ppid = static_cast<int>(entry.th32ParentProcessID);
      // Live enough to be in the snapshot; see the header note on zombies.
      info.state = 'R';
      found.push_back(info);
    } while (Process32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return found;
}

inline SweepResult SweepOrphans(const std::string&, const std::string&, bool) {
  SweepResult result;
  result.ran = true;
  result.supported = false;  // nothing leaks here - see the header note
  return result;
}

inline std::string SelfPath(const char* argv0) {
  char buffer[MAX_PATH];
  const DWORD n = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return std::string(buffer, n);
  return argv0 ? std::string(argv0) : std::string();
}

}  // namespace launcher::platform
