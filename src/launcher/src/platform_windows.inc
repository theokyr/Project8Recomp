// Windows implementation of platform.h. Included by platform.cpp; not a
// standalone translation unit.
//
// UNTESTED ON REAL HARDWARE as of v0.1.0. This side is written from the Win32
// documentation and exercised under Wine in CI; nobody on the project owns a
// Windows machine. Every place where that matters is marked. The launcher is
// published for Windows as an experimental artifact for exactly this reason -
// see the platform table in the README.

#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>

namespace thps::platform {
namespace {

HANDLE AsHandle(std::uint64_t v) { return reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(v)); }
std::uint64_t AsSlot(HANDLE h) { return static_cast<std::uint64_t>(reinterpret_cast<UINT_PTR>(h)); }

// CreateProcess takes one command line, not an argv, and does its own splitting
// on the other side. These are the MSVC/CRT rules the default parser follows -
// backslashes are only special immediately before a quote, which is why a
// trailing separator in a path (`C:\install\`) has to be doubled and a
// mid-path one must not be.
//
// Getting this wrong is the classic Windows-port bug and it fails on exactly
// the inputs a user supplies: paths with spaces, and paths ending in a
// separator. Both are normal here - the install directory is user-chosen and
// the disc image path comes from a file dialog.
std::string QuoteArg(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
    return arg;
  }
  std::string out = "\"";
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      // Before the closing quote: every backslash must be escaped, or it would
      // escape the quote instead of standing for itself.
      out.append(backslashes * 2, '\\');
      break;
    }
    if (arg[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back('"');
  return out;
}

std::string BuildCommandLine(const std::vector<std::string>& argv) {
  std::string line;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i) line.push_back(' ');
    line += QuoteArg(argv[i]);
  }
  return line;
}

}  // namespace

Child::~Child() {
  if (!valid_) return;
  if (handles_[0]) CloseHandle(AsHandle(handles_[0]));  // read end of the pipe
  if (handles_[1]) CloseHandle(AsHandle(handles_[1]));  // process
}

Child::Child(Child&& other) noexcept { *this = std::move(other); }

Child& Child::operator=(Child&& other) noexcept {
  if (this == &other) return *this;
  valid_ = other.valid_;
  handles_[0] = other.handles_[0];
  handles_[1] = other.handles_[1];
  pid_ = other.pid_;
  other.valid_ = false;
  other.handles_[0] = other.handles_[1] = 0;
  other.pid_ = -1;
  return *this;
}

Child Start(const std::vector<std::string>& argv) {
  Child child;
  if (argv.empty()) return child;

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 0)) return child;
  // Only the write end may cross into the child. Leaving the read end
  // inheritable is the reason a pipe read hangs forever after the child exits:
  // the child holds a copy open and the read never sees EOF.
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_end;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi{};
  std::string command = BuildCommandLine(argv);
  const BOOL ok = CreateProcessA(argv[0].c_str(), command.data(), nullptr, nullptr,
                                 /*bInheritHandles=*/TRUE, /*flags=*/0, nullptr, nullptr, &si, &pi);
  CloseHandle(write_end);
  if (!ok) {
    CloseHandle(read_end);
    std::fprintf(stderr, "thps_p8_gui: could not start %s (error %lu)\n", argv[0].c_str(),
                 GetLastError());
    return child;
  }
  CloseHandle(pi.hThread);

  child.handles_[0] = AsSlot(read_end);
  child.handles_[1] = AsSlot(pi.hProcess);
  child.pid_ = int(pi.dwProcessId);
  child.valid_ = true;
  return child;
}

int Child::Drain(const std::function<void(const std::string&)>& on_line) {
  if (!valid_) return -1;
  HANDLE read_end = AsHandle(handles_[0]);
  HANDLE process = AsHandle(handles_[1]);

  std::string pending;
  char buf[4096];
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(read_end, buf, DWORD(sizeof(buf)), &got, nullptr) || got == 0) {
      break;  // ERROR_BROKEN_PIPE is the normal end: the child closed stdout
    }
    pending.append(buf, size_t(got));
    size_t start = 0;
    for (;;) {
      const size_t nl = pending.find('\n', start);
      if (nl == std::string::npos) break;
      std::string line = pending.substr(start, nl - start);
      // The worker writes "\n"; a CRT in text mode on the other side makes it
      // "\r\n". Strip it here rather than in every parser downstream, which is
      // where a stray \r otherwise surfaces - as a sscanf that silently reads
      // the wrong number of fields.
      if (!line.empty() && line.back() == '\r') line.pop_back();
      on_line(line);
      start = nl + 1;
    }
    pending.erase(0, start);
  }
  if (!pending.empty()) {
    if (pending.back() == '\r') pending.pop_back();
    on_line(pending);
  }

  WaitForSingleObject(process, INFINITE);
  DWORD code = 0;
  if (!GetExitCodeProcess(process, &code)) return -1;
  // TerminateProcess was asked for with this code; report it as "stopped"
  // rather than as an exit status, so a cancel is never read as a failure.
  if (code == 0xC000013AU || code == 1u + 0x40000000U) return -1;
  return int(code);
}

void Child::RequestStop() {
  if (!valid_ || !handles_[1]) return;
  // No SIGTERM equivalent. TerminateProcess is abrupt, which is acceptable for
  // exactly the reason the POSIX side uses a polite signal and does not need
  // to escalate: the child is a file-copying worker with nothing to flush, and
  // the partial output is deleted by the caller either way.
  TerminateProcess(AsHandle(handles_[1]), 1u + 0x40000000U);
}

void RequestStopByPid(int pid) {
  if (pid <= 0) return;
  // The game, not the extraction worker. This one DOES have state worth
  // flushing - it writes a profile and closes a recording on the way out - and
  // Windows offers no way to ask a windowed process to do that from outside
  // without a console it does not have.
  //
  // UNVERIFIED: on Linux this path is SIGTERM and the game's handler runs. The
  // Windows equivalent would be the game installing a control handler and this
  // sending WM_CLOSE to its top-level window. Until a Windows game build exists
  // to test that against, this is a hard stop and the launcher's Stop button is
  // documented as such.
  if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid))) {
    TerminateProcess(h, 1);
    CloseHandle(h);
  }
}

bool OpenFolder(const std::filesystem::path& path) {
  const std::string target = path.string();
  const HINSTANCE rc = ShellExecuteA(nullptr, "open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  // ShellExecute returns a value >32 on success. It is not a handle despite the
  // type; this is the documented convention.
  return reinterpret_cast<INT_PTR>(rc) > 32;
}

[[noreturn]] void ExecReplace(const std::vector<std::string>& argv) {
  // Windows has no execve that replaces the image in place. `_execv` exists in
  // the CRT but spawns and terminates, which loses the process identity that
  // `thps_p8_launch`'s single-instance check and any parent shell rely on.
  //
  // So this spawns and exits explicitly rather than pretending. The difference
  // from POSIX that matters: this process is gone before the child finishes
  // starting, so on the Windows side the launcher does NOT hold the window's
  // GPU handles while the game runs - that concern is answered by the shutdown
  // having already happened, not by the exec.
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::string command = BuildCommandLine(argv);
  const BOOL ok = CreateProcessA(argv[0].c_str(), command.data(), nullptr, nullptr,
                                 /*bInheritHandles=*/FALSE, /*flags=*/0, nullptr, nullptr, &si, &pi);
  if (!ok) {
    std::fprintf(stderr, "thps_p8_gui: could not start %s (error %lu)\n", argv[0].c_str(),
                 GetLastError());
    ExitProcess(1);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  ExitProcess(0);
}

std::filesystem::path SiblingExe(const std::filesystem::path& dir, const char* stem) {
  return dir / (std::string(stem) + ".exe");
}

}  // namespace thps::platform
