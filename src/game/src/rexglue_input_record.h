// rexglue_input_record.h - records the input reaching guest pad slot 0 into a
// streamed JSONL file that rexglue_script_input.h can replay.
//
// Drop it in next to rexglue_script_input.h and wire it with one more line,
// AFTER the script layer (order matters - see Install()):
//
//   void OnPreSetup(rex::RuntimeConfig& config) override {
//     rexglue_script_input::Install(config);
//     rexglue_input_record::Install(config);
//   }
//
// and, if the app installs a SIGTERM/SIGINT handler that does not return,
// one call inside it:
//
//   rexglue_input_record::FlushOnSignal();
//
// The app then accepts:
//
//   --input_record_file=/path/to/session.jsonl
//   --input_mark_names=loading,gameplay,ragdoll,settled
//
// Default empty = nothing is installed, nothing is opened, no wrapper driver
// exists and the input stack is byte-for-byte what it was without this header.
//
// ------------------------------------------------------------------- marks
//
// MarkNext() writes a `{"type":"mark",...}` line naming the current moment on
// the recording's own timebase, taking the next name from --input_mark_names.
// Bind it to a key (the app binds F9) and a phase boundary can be stamped the
// instant it is seen, instead of being timed by hand off a video afterwards.
//
// The point is what happens next: rexglue_script_input.h reads those lines
// back, so every later replay of the take re-announces the same boundaries at
// the same times, and a reporting tool can line them up against the per-frame
// counters in frames.csv. One press converts an observation into a
// fixture-carried fact.
//
// ------------------------------------------------------------- what it taps
//
// The SDK's InputSystem::GetState merges every driver (SDL pad, MnK keyboard,
// the scripted pad) and hands the guest one X_INPUT_STATE. A driver cannot see
// that merge - it only contributes to it - so recording from a driver slot
// would capture one source and miss the rest. Instead the whole real input
// system is nested inside a single tap driver held by a one-driver outer
// system:
//
//   outer InputSystem -> RecordTapDriver -> inner InputSystem
//                                             -> SDL / MnK / NOP drivers
//                                             -> ScriptInputDriver
//
// The tap forwards every call through and records what comes back, so what
// lands in the file is exactly the state the guest was handed - real pad, real
// keyboard and scripted playback alike, already merged. Forwarding is total:
// GetCapabilities/SetState/GetKeystroke pass straight through, window
// attachment is forwarded to the inner system, and the inner drivers' active
// callback is chained to the tap's own, so focus-gating (the MnK driver zeroes
// itself when the window is not focused) keeps working exactly as before.
//
// -------------------------------------------------------- crash durability
//
// One JSON object per line, written with a single unbuffered O_APPEND write(),
// and only when the pad state actually changes. There is no userspace buffer
// to lose: SIGTERM, Alt+F4, a hard kill or a runtime crash cost at most the
// line being written at that instant, and the replay loader skips a torn tail
// line with a warning rather than refusing the file. Clean shutdown adds a
// neutral sample (so replay ends with hands off the pad) and an "end" record,
// then fsyncs. FlushOnSignal() only fsyncs, which is async-signal-safe.

#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "rexglue_script_input.h"

REXCVAR_DEFINE_STRING(input_record_file, "", "Input",
                      "Path to stream a JSONL recording of guest pad slot 0 to "
                      "(replayable with --input_json_file)");
REXCVAR_DEFINE_STRING(input_mark_names, "", "Input",
                      "Comma-separated names handed out to successive mark "
                      "presses while recording (empty, or once exhausted: "
                      "mark1, mark2, ...)");

namespace rexglue_input_record {

// The SDK's X_ERROR_* macros expand to casts naming the unqualified types, so
// alias them into this namespace the same way the script layer does.
using X_RESULT = rex::X_RESULT;
using X_STATUS = rex::X_STATUS;

using rexglue_script_input::NowMs;
using rexglue_script_input::Sample;

inline constexpr int kFormatVersion = 1;

// Append-only JSONL writer. One write() per line, no buffering, no formatting
// in the signal path.
class Writer {
 public:
  bool Open(const std::string& path) {
#ifdef _WIN32
    REXLOG_ERROR("input-record: recording needs POSIX file APIs; not supported "
                 "on this platform");
    return false;
#else
    std::lock_guard lock(mutex_);
    if (fd_.load() >= 0) return true;
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC,
                    0644);
    if (fd < 0) {
      REXLOG_ERROR("input-record: cannot open --input_record_file={} ({})", path,
                   std::strerror(errno));
      return false;
    }
    fd_.store(fd);
    path_ = path;
    char utc[32] = "";
    std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
    if (gmtime_r(&now, &tm_utc)) {
      std::strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    }
    char line[512];
    int n = std::snprintf(
        line, sizeof(line),
        "{\"type\":\"header\",\"format\":\"rexglue-input-jsonl\",\"version\":%d,"
        "\"slot\":0,\"timebase\":\"ms_since_input_system_create\","
        "\"units\":\"buttons=xinput mask, lx/ly/rx/ry=-32768..32767, lt/rt=0..255\","
        "\"created_utc\":\"%s\"}\n",
        kFormatVersion, utc);
    WriteLineLocked(line, size_t(n));
    REXLOG_INFO("input-record: recording guest pad slot 0 to {}", path);
    return true;
#endif
  }

  bool is_open() const { return fd_.load() >= 0; }

  // Read from the UI thread every frame by the debug panel, so these are
  // lock-free rather than mutex-guarded: `path_` is written once inside Open()
  // before the first sample can arrive and never mutated after, and the two
  // counters are display values where a torn read cannot mean anything worse
  // than a number one behind.
  const std::string& path() const { return path_; }
  uint64_t sample_count() const { return samples_.load(std::memory_order_relaxed); }
  uint64_t mark_count() const { return marks_.load(std::memory_order_relaxed); }

  // Called from guest polling threads on every slot-0 GetState. Writes only
  // when the state actually differs from the last line written.
  void Observe(const Sample& s) {
    std::lock_guard lock(mutex_);
    if (fd_.load() < 0) return;
    if (have_last_ && s.SameState(last_)) return;
    last_ = s;
    have_last_ = true;
    WriteSampleLocked(s);
  }

  // Writes a named moment onto the recording's own timebase. Called from the
  // UI thread by the mark keybind while the guest threads are writing samples,
  // hence the same lock: one write(), same crash durability as a sample line.
  // Returns false when there is no recording open, which is the interesting
  // case to report - marking during a plain `make play` is a no-op the user
  // needs to be told about, not a silent nothing.
  bool Mark(const std::string& name) {
    std::lock_guard lock(mutex_);
    if (fd_.load() < 0) return false;
    int64_t t = NowMs();
    char line[256];
    int n = std::snprintf(line, sizeof(line),
                          "{\"type\":\"mark\",\"t\":%lld,\"name\":\"%s\"}\n",
                          (long long)t, name.c_str());
    if (n <= 0) return false;
    WriteLineLocked(line, size_t(std::min<size_t>(size_t(n), sizeof(line) - 1)));
    marks_.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("input-record: mark '{}' t={}ms", name, t);
    return true;
  }

  // Clean shutdown: park the pad, close the record, get it on disk.
  void Close() {
    std::lock_guard lock(mutex_);
    int fd = fd_.load();
    if (fd < 0) return;
    if (have_last_ && !last_.IsNeutral()) {
      Sample neutral;
      neutral.t_ms = NowMs();
      last_ = neutral;
      WriteSampleLocked(neutral);
    }
    const uint64_t samples = samples_.load(std::memory_order_relaxed);
    const uint64_t marks = marks_.load(std::memory_order_relaxed);
    char line[128];
    int n = std::snprintf(
        line, sizeof(line),
        "{\"type\":\"end\",\"t\":%lld,\"samples\":%llu,\"marks\":%llu}\n",
        (long long)NowMs(), (unsigned long long)samples,
        (unsigned long long)marks);
    WriteLineLocked(line, size_t(n));
#ifndef _WIN32
    ::fsync(fd);
    fd_.store(-1);
    ::close(fd);
#endif
    REXLOG_INFO("input-record: wrote {} samples and {} mark(s) to {}", samples,
                marks, path_);
  }

  // Async-signal-safe: fsync(2) only - no locking, no allocation, no
  // formatting. Every complete line is already in the kernel, so this only
  // has to get them onto the disk.
  void SignalFlush() {
#ifndef _WIN32
    int fd = fd_.load();
    if (fd >= 0) ::fsync(fd);
#endif
  }

 private:
  void WriteSampleLocked(const Sample& s) {
    std::string names = rexglue_script_input::detail::ButtonNames(s.buttons);
    char line[256];
    int n = std::snprintf(
        line, sizeof(line),
        "{\"type\":\"s\",\"t\":%lld,\"b\":%u,\"buttons\":\"%s\",\"lx\":%d,\"ly\":%d,"
        "\"rx\":%d,\"ry\":%d,\"lt\":%u,\"rt\":%u}\n",
        (long long)s.t_ms, unsigned(s.buttons), names.c_str(), int(s.axes[0]),
        int(s.axes[1]), int(s.axes[2]), int(s.axes[3]), unsigned(s.triggers[0]),
        unsigned(s.triggers[1]));
    if (n <= 0) return;
    WriteLineLocked(line, size_t(std::min<size_t>(size_t(n), sizeof(line) - 1)));
    ++samples_;
  }

  void WriteLineLocked(const char* data, size_t len) {
#ifndef _WIN32
    int fd = fd_.load();
    if (fd < 0) return;
    size_t off = 0;
    while (off < len) {
      ssize_t w = ::write(fd, data + off, len - off);
      if (w > 0) {
        off += size_t(w);
        continue;
      }
      if (w < 0 && errno == EINTR) continue;
      if (!write_failed_) {
        write_failed_ = true;
        REXLOG_ERROR("input-record: write failed ({}); recording stops here",
                     std::strerror(errno));
      }
      fd_.store(-1);
      ::close(fd);
      return;
    }
#else
    (void)data;
    (void)len;
#endif
  }

  std::mutex mutex_;
  std::atomic<int> fd_{-1};
  std::string path_;
  Sample last_;
  bool have_last_ = false;
  bool write_failed_ = false;
  std::atomic<uint64_t> samples_{0};
  std::atomic<uint64_t> marks_{0};
};

// Process singleton: the tap driver writes to it from guest threads and the
// signal handler reaches it without an object to hand.
inline Writer& Instance() {
  static Writer writer;
  return writer;
}

// Safe to call from a SIGTERM/SIGINT handler, including one that _exit()s.
inline void FlushOnSignal() {
  Instance().SignalFlush();
}

// Stamps the next name from --input_mark_names into the open recording. Bound
// to a key by the app, so the moment a phase boundary is observed it becomes a
// timestamp on the same clock the replay runs on - which is what makes the
// boundary survive into every later replay of that take instead of living in
// someone's notes.
//
// Names are consumed in order and the counter advances even when nothing is
// recording, so a given press always means the same phase whether or not it
// landed. Past the end of the list, and when the list is empty, marks are
// numbered.
// How many times MarkNext() has been called. Advances even when nothing is
// recording, so a given press always means the same phase whether or not it
// landed - the alternative silently renames every later mark of a session that
// started before the recording opened.
inline std::atomic<size_t>& MarkPressCount() {
  static std::atomic<size_t> pressed{0};
  return pressed;
}

// The name the Nth press gets: the Nth non-empty field of --input_mark_names,
// or "markN+1" past the end of the list (and whenever the list is empty).
inline std::string MarkNameAt(size_t index) {
  std::string names = REXCVAR_QUERY(std::string, input_mark_names);
  size_t begin = 0, seen = 0;
  while (begin <= names.size()) {
    size_t end = names.find(',', begin);
    if (end == std::string::npos) end = names.size();
    std::string field = names.substr(begin, end - begin);
    // Trim: this list gets typed by a human at a shell prompt.
    while (!field.empty() && std::isspace((unsigned char)field.front()))
      field.erase(field.begin());
    while (!field.empty() && std::isspace((unsigned char)field.back()))
      field.pop_back();
    if (!field.empty()) {
      if (seen == index) {
        return rexglue_script_input::detail::SanitizeMarkName(field);
      }
      ++seen;
    }
    if (end == names.size()) break;
    begin = end + 1;
  }
  return "mark" + std::to_string(index + 1);
}

// The name the NEXT press would get, without consuming it. For the panel.
inline std::string PeekNextMarkName() {
  return MarkNameAt(MarkPressCount().load(std::memory_order_relaxed));
}

inline void MarkNext() {
  size_t index = MarkPressCount().fetch_add(1, std::memory_order_relaxed);
  std::string name = MarkNameAt(index);
  if (!Instance().Mark(name)) {
    REXLOG_WARN("input-record: mark '{}' ignored - this run is not recording "
                "(--input_record_file is unset; use `make record`)",
                name);
  }
  // Observers fire whether or not the mark landed in a file: F9 means "sample
  // the run here", and a screenshot of the moment is worth having even in a
  // session that is not recording. A live press and the replay of the mark it
  // wrote therefore produce the same artefact, which is the whole point of
  // comparing a take against its replays.
  rexglue_script_input::NotifyMark(index + 1, name,
                                   rexglue_script_input::NowMs());
}

// Nests the real input system inside one driver so the tap sees the merged
// state the guest sees. Every override forwards.
class RecordTapDriver : public rex::input::InputDriver {
 public:
  explicit RecordTapDriver(std::unique_ptr<rex::input::InputSystem> inner)
      : rex::input::InputDriver(nullptr, 0), inner_(std::move(inner)) {
    // Chain the inner drivers' focus gate to this driver's own, which the
    // outer system sets later. Without this the MnK driver would lose its
    // "window is not focused" check and start reading the desktop's keyboard.
    inner_->SetActiveCallback([this] { return this->is_active(); });
  }

  ~RecordTapDriver() override { Instance().Close(); }

  rex::X_STATUS Setup() override { return inner_->Setup(); }

  rex::X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                                rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    return inner_->GetCapabilities(user_index, flags, out_caps);
  }

  rex::X_RESULT GetState(uint32_t user_index,
                         rex::input::X_INPUT_STATE* out_state) override {
    rex::input::X_INPUT_STATE state{};
    rex::X_RESULT result = inner_->GetState(user_index, &state);
    if (user_index == 0 && result == X_ERROR_SUCCESS) {
      Sample s;
      s.t_ms = NowMs();
      s.buttons = static_cast<uint16_t>(state.gamepad.buttons);
      s.axes[0] = static_cast<int16_t>(state.gamepad.thumb_lx);
      s.axes[1] = static_cast<int16_t>(state.gamepad.thumb_ly);
      s.axes[2] = static_cast<int16_t>(state.gamepad.thumb_rx);
      s.axes[3] = static_cast<int16_t>(state.gamepad.thumb_ry);
      s.triggers[0] = state.gamepad.left_trigger;
      s.triggers[1] = state.gamepad.right_trigger;
      Instance().Observe(s);
    }
    if (out_state) *out_state = state;
    return result;
  }

  rex::X_RESULT SetState(uint32_t user_index,
                         rex::input::X_INPUT_VIBRATION* vibration) override {
    return inner_->SetState(user_index, vibration);
  }

  rex::X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                             rex::input::X_INPUT_KEYSTROKE* out_keystroke) override {
    return inner_->GetKeystroke(user_index, flags, out_keystroke);
  }

  void OnWindowAvailable(rex::ui::Window* window) override {
    inner_->AttachWindow(window);
  }

 private:
  std::unique_ptr<rex::input::InputSystem> inner_;
};

// Wraps config.input_factory. Install this AFTER rexglue_script_input::Install
// so the scripted pad is inside the tap and therefore part of what is
// recorded - which is what lets a fixture run be recorded and replayed.
// Touches nothing when --input_record_file is unset.
inline void Install(rex::RuntimeConfig& config) {
  auto prev = config.input_factory;
  config.input_factory =
      [prev](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    (void)rexglue_script_input::Origin();
    std::unique_ptr<rex::system::IInputSystem> built =
        prev ? prev(tool_mode) : rex::input::CreateDefaultInputSystem(tool_mode);
    std::string path = REXCVAR_QUERY(std::string, input_record_file);
    if (path.empty() || !built) return built;
    // Every factory in this SDK yields a rex::input::InputSystem; the runtime
    // itself static_casts to it to call AttachWindow, so this is the same
    // assumption, not a new one.
    std::unique_ptr<rex::input::InputSystem> inner(
        static_cast<rex::input::InputSystem*>(built.release()));
    if (!Instance().Open(path)) {
      // Recording is best-effort: a run must never fail because a log did.
      return inner;
    }
    auto outer = std::make_unique<rex::input::InputSystem>(nullptr);
    outer->AddDriver(std::make_unique<RecordTapDriver>(std::move(inner)));
    return outer;
  };
}

}  // namespace rexglue_input_record
