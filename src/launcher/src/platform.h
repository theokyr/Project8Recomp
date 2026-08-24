// Everything the launcher does that is not drawing.
//
// This header exists because of a shape the launcher had before it: `popen`,
// `kill(pid, SIGTERM)`, `execv`, `xdg-open` and `std::system` with a shell
// quoting convention, all inlined into the UI class. Every one of those is a
// POSIX call with a Win32 answer that is *not* a rename, and leaving them
// inline meant the Windows port could only ever be a rewrite of the UI.
//
// Three of them also had real defects worth naming, because they are the
// reason this is an interface rather than a set of #ifdefs at each call site:
//
//   - `popen` gives no pid, so a 4.7 GB extraction could not be cancelled. The
//     UI had no Cancel button because there was no way to implement one.
//   - Every command was built by pasting a user-supplied path into a string and
//     handing it to a shell. The path comes from a file dialog, so the practical
//     risk was low, but a filename containing a double quote was enough to break
//     it. Nothing here goes through a shell now; argv is a vector.
//   - `std::system("xdg-open ... &")` reports the shell's exit status, not the
//     opener's, so the failure branch could never fire.
//
// The contract is deliberately small - run a child and read its stdout, stop a
// child, ask about free space, open a folder, replace this process - because
// every entry is a thing that has to be written twice.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace thps::platform {

// A child process the caller can still stop. Opaque: the POSIX side holds a pid
// and a pipe fd, the Windows side holds two HANDLEs, and the UI holds neither.
class Child {
 public:
  Child() = default;
  ~Child();
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  Child(Child&&) noexcept;
  Child& operator=(Child&&) noexcept;

  bool valid() const { return valid_; }

  // Reads the child's stdout to completion, calling `on_line` for each line as
  // it arrives - which is what makes a progress bar possible at all; the
  // buffered-to-completion version could only ever show 0% and then 100%.
  //
  // `on_line` runs on the calling thread, which is the extraction worker, never
  // the UI thread.
  //
  // Returns the exit code, or -1 if the child was stopped or never ran.
  int Drain(const std::function<void(const std::string&)>& on_line);

  // Asks the child to stop, and does not wait. Safe to call from another thread
  // while Drain is running: Drain's read then ends and it returns -1.
  //
  // A polite stop on both platforms (SIGTERM / TerminateProcess). The child is
  // a file-copying worker with no state worth flushing, so there is nothing to
  // be gained by a two-stage escalation and something to lose in complexity.
  void RequestStop();

 private:
  friend Child Start(const std::vector<std::string>&);
  bool valid_ = false;
  // Platform state. Sized rather than typed so this header pulls in no OS
  // headers - the launcher's translation units include it and RmlUi both, and
  // <windows.h> in that mix is a well-known way to lose an afternoon.
  std::uint64_t handles_[2] = {0, 0};
  int pid_ = -1;
};

// Starts `argv[0]` with `argv` and a pipe on its stdout. No shell is involved,
// so no argument needs quoting and none is interpreted.
//
// The child's stderr is left attached to ours: the SDK-side worker logs there
// on purpose (its stdout is the machine-readable channel), and those lines are
// the only diagnostic anyone gets when a first run fails on someone else's
// machine.
Child Start(const std::vector<std::string>& argv);

// Runs to completion and returns stdout. The convenience form of Start+Drain
// for the two calls that are over in milliseconds and cannot be cancelled
// meaningfully - identity and `--check`.
struct RunResult {
  bool ran = false;  // the process itself started
  int exit_code = -1;
  std::string out;
};
RunResult Run(const std::vector<std::string>& argv);

// Asks a process to stop. Used for the game, whose pids come from
// `thps_p8_launch --check --json` - the launcher never goes looking for
// processes itself.
//
// SIGTERM rather than SIGKILL on POSIX: the game's own handler flushes a
// recording and writes its profile before leaving.
void RequestStopByPid(int pid);

// Free space on the volume holding `path`, or on its nearest existing parent -
// the extraction target does not exist yet when this is asked.
//
// Returns 0 when it cannot be determined, and callers must treat 0 as "do not
// know" rather than "no room": refusing to start because a query failed would
// turn a working install into a broken one.
std::uint64_t FreeSpaceBytes(const std::filesystem::path& path);

// Opens a folder in the desktop's file manager. Best-effort by nature; returns
// false when the platform said no, which is the only case worth a message.
bool OpenFolder(const std::filesystem::path& path);

// Replaces this process with `argv`. Does not return on success.
//
// Replace, not spawn: the launcher is one-shot and never comes back, so there
// is nothing for it to wait for, and replacing the image means no orphaned
// parent is left holding the window's GPU file descriptors while the game
// runs. Windows has no execve worth the name, so that side
// spawns and exits - documented at the implementation, because it is a real
// behavioural difference and the fd-inheritance question has to be answered
// differently there.
[[noreturn]] void ExecReplace(const std::vector<std::string>& argv);

// The executable's own directory. The portable install is "wherever this
// binary lives", so this is the root of everything.
std::filesystem::path SelfDir();

// Platform-appropriate name for a sibling executable: `thps_p8_launch` on
// POSIX, `thps_p8_launch.exe` on Windows. Every path the launcher builds to a
// sibling binary goes through this.
std::filesystem::path SiblingExe(const std::filesystem::path& dir, const char* stem);

}  // namespace thps::platform
