// rexglue_script_input.h - portable scripted-input layer for ReXGlue recomps.
//
// Drop this header into any rexglue-sdk app and wire it with two lines - no
// SDK modification, no OS-level input injection, no virtual devices:
//
//   #include "rexglue_script_input.h"          // once, in the app TU
//   ...
//   void OnPreSetup(rex::RuntimeConfig& config) override {
//     rexglue_script_input::Install(config);
//   }
//
// The app then accepts (underscore cvar syntax, like every runtime flag):
//
//   --input_json='[{"action":"delay","value":14000},
//                  {"action":"button","value":"start"},
//                  {"action":"delay","value":1000},
//                  {"action":"button","value":"a","hold":250},
//                  {"action":"axis","value":"ly","position":-32768,"hold":3000}]'
//   --input_json_file=/path/to/script.json     (same content, from a file)
//
// Script model: a SEQUENTIAL timeline on virtual pad slot 0, starting when
// the runtime's input system is created (a few hundred ms after process
// launch). Entries:
//
//   {"action":"delay",  "value":MS}                          advance time
//   {"action":"button", "value":NAME, "hold":MS=150}         press+release
//   {"action":"axis",   "value":"lx|ly|rx|ry",
//                       "position":-32768..32767, "hold":MS=150}
//   {"action":"trigger","value":"lt|rt", "position":0..255, "hold":MS=150}
//   {"action":"command","value":"cheat_position"}      run project command
//
// Buttons: a b x y start back lb rb l3 r3 up down left right guide.
// Actions overlap only via explicit timing: each non-delay entry advances the
// cursor by its hold, so consecutive entries are sequential; use "at":MS to
// schedule an entry at an absolute time instead (does not advance the cursor).
// The scripted pad merges with real pads/keyboard through the SDK's normal
// multi-driver state merge, so it composes rather than replaces.
//
// ---------------------------------------------------------------- JSONL
//
// The same cvars also accept the streamed recording format written by
// rexglue_input_record.h - one JSON object per line, no enclosing array:
//
//   {"type":"header","format":"rexglue-input-jsonl","version":1, ...}
//   {"type":"s","t":1200,"b":0,"buttons":"","lx":0,...,"lt":0,"rt":0}
//   {"type":"s","t":1350,"b":4096,"buttons":"a",...}
//   {"type":"mark","t":12000,"name":"gameplay"}
//   {"type":"end","t":42000,"samples":317}
//
// A "s" line is an absolute-time *state snapshot* of pad slot 0 rather than an
// action: the pad holds that state from "t" until the next "s" line. "b" is
// the authoritative button mask and "buttons" the same bits spelled out for
// hand-editing (the loader prefers "b" when both are present, so delete "b"
// after editing names). Which format a file is in is sniffed from its first
// non-space character - '[' is the action array, '{' is JSONL - so .json
// fixtures and .jsonl recordings are interchangeable wherever a path is taken.
// Both share one timebase: Origin(), pinned when the input system is created.
// A malformed line is skipped with a warning rather than failing the load,
// which is what makes a recording truncated by SIGKILL still replayable.
//
// ---------------------------------------------------------------- marks
//
// A "mark" record names a moment on that same timebase:
//
//   {"type":"mark","t":12000,"name":"gameplay"}        (jsonl)
//   {"action":"mark","value":"gameplay"}               (action array)
//
// It contributes nothing to the pad. When the clock reaches it, the replay
// logs one line at info level:
//
//   script-input: mark 'gameplay' t=12000ms
//
// That is the whole point: a phase boundary someone observed by eye becomes a
// timestamped record that replays identically every run and can be read back
// out of the log by a tool. `rexglue_input_record.h` writes these live from a
// keybind, so a boundary can be marked while playing and is then carried by
// the recording into every replay of it. A reporting tool can then reconcile
// those marks against the per-frame counters in frames.csv.
//
// A file with marks and no pad input is not playable input; it still loads,
// and the marks still fire. Cost when a file has no marks is one relaxed
// atomic load per GetState.
//
// ------------------------------------------------------------- mark observers
//
// A mark is the one moment in a run that somebody already decided is worth
// looking at, which makes it the natural trigger for anything that wants to
// sample the run: a screenshot, a counter dump, a log fence. Rather than teach
// this header about any of those, SetMarkObserver() publishes the event and
// lets the app decide what a mark costs. `thps_p8_app.h` hangs the
// guest-output screenshot service off it, so every marked fixture yields one
// PNG per mark with no per-fixture wiring at all.
//
// The observer runs INLINE on whichever thread reached the mark - a guest input
// poll for a replayed mark, the UI thread for a live F9 press - so it must
// queue work rather than do it.
//
// ---------------------------------------------------------- command observers
//
// A command event is the control-plane counterpart to a mark:
//
//   {"action":"command","value":"cheat_teleport 1 2 3","at":62000}
//
// It is zero-duration, honours `at`, and never changes the input cursor. The
// portable input layer publishes the text through SetCommandObserver(); the
// title decides which command grammar receives it. If the observer is not yet
// installed, the event remains pending instead of being silently dropped.

#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_STRING(input_json, "", "Input",
                      "JSON array of scripted pad inputs to play back on slot 0");
REXCVAR_DEFINE_BOOL(input_poll_clock, false, "Input",
                    "Drive the replay from a per-poll virtual clock instead of host wall time, "
                    "so every entry lands on the same guest frame in every run "
                    "(reproducible benchmarks)");
REXCVAR_DEFINE_INT32(input_poll_clock_after_ms, 18000, "Input",
                     "Wall-clock time on the INPUT timebase at which input_poll_clock hands "
                     "over from wall time to poll time. Must be after the fixture reaches "
                     "gameplay: menus stay on the wall clock because the poll rate is not "
                     "frame-locked while loading, and handing over early stretches the menu "
                     "inputs - at 12000 this fixture walked into the create-a-skater screen. "
                     "freeskate-fast marks 'gameplay' at 17065 ms, hence 18000. Note the input "
                     "timebase runs ~5.8 s ahead of the frames.csv clock");
REXCVAR_DEFINE_INT32(input_poll_hz, 120, "Input",
                     "Polls per virtual second for input_poll_clock. This title polls pad 0 "
                     "twice per rendered frame, so 120 makes the virtual clock match wall time "
                     "at a locked 60 fps");

REXCVAR_DEFINE_STRING(input_json_file, "", "Input",
                      "Path to a file holding the same JSON as --input_json");

namespace rexglue_script_input {

// The SDK's X_ERROR_* / X_STATUS_SUCCESS macros expand to casts naming the
// unqualified types, so alias them into this namespace.
using X_RESULT = rex::X_RESULT;
using X_STATUS = rex::X_STATUS;

// Shared zero for every timeline in the process, scripted or recorded: the
// first call, which Install()'s factory makes before it constructs a driver -
// i.e. when the runtime creates the input system, a few hundred ms into the
// process. Recording and replay therefore measure "t" from the same event,
// which is the whole reason a recording replays where it was recorded.
inline std::chrono::steady_clock::time_point Origin() {
  static const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  return t0;
}

inline int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - Origin())
      .count();
}

// One absolute-time snapshot of pad slot 0 - the JSONL "s" line, and the unit
// both the recorder writes and the replay path holds between samples.
struct Sample {
  int64_t t_ms = 0;
  uint16_t buttons = 0;
  int16_t axes[4] = {0, 0, 0, 0};  // lx ly rx ry
  uint8_t triggers[2] = {0, 0};    // lt rt

  bool SameState(const Sample& o) const {
    return buttons == o.buttons && axes[0] == o.axes[0] && axes[1] == o.axes[1] &&
           axes[2] == o.axes[2] && axes[3] == o.axes[3] &&
           triggers[0] == o.triggers[0] && triggers[1] == o.triggers[1];
  }
  bool IsNeutral() const { return SameState(Sample{}); }
};

// A named moment on the shared timebase. Carries no pad state - it exists so
// that "this is where gameplay started" survives as data instead of as a
// number someone wrote down while watching.
struct Mark {
  int64_t t_ms = 0;
  std::string name;
};

struct Command {
  int64_t t_ms = 0;
  std::string text;
};

// Called once per mark, by whichever thread reached it. Arguments are the
// 1-based ordinal within the run, the sanitized name, and the mark's time on
// the shared input timebase. See the "mark observers" note at the top: this
// runs inline on a guest polling thread, so it must not block.
using MarkObserver =
    std::function<void(size_t ordinal, const std::string& name, int64_t t_ms)>;
using CommandObserver =
    std::function<void(size_t ordinal, const std::string& text, int64_t t_ms)>;
using PollObserver = std::function<void()>;

namespace detail {

// One observer per process, guarded by a mutex that is only ever taken when a
// mark actually fires (a handful of times per run). The atomic is what the
// no-observer path pays: one acquire load, and only once a mark is due.
struct ObserverState {
  std::mutex mutex;
  MarkObserver fn;
  std::atomic<bool> present{false};
};

inline ObserverState& mark_observer() {
  static ObserverState state;
  return state;
}

struct CommandObserverState {
  std::mutex mutex;
  CommandObserver fn;
  std::atomic<bool> present{false};
};

inline CommandObserverState& command_observer() {
  static CommandObserverState state;
  return state;
}

}  // namespace detail

// Install (or, with an empty function, remove) the process mark observer.
// Removal is what shutdown needs: the observer captures app state that dies
// before the input system does.
inline void SetMarkObserver(MarkObserver fn) {
  detail::ObserverState& state = detail::mark_observer();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.fn = std::move(fn);
  state.present.store(bool(state.fn), std::memory_order_release);
}

// Fire the observer for one mark. Called by the replay driver when the clock
// reaches a mark, and by rexglue_input_record.h when a live F9 stamps one, so
// a mark means the same thing whether it is being recorded or replayed.
inline void NotifyMark(size_t ordinal, const std::string& name, int64_t t_ms) {
  detail::ObserverState& state = detail::mark_observer();
  if (!state.present.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.fn) state.fn(ordinal, name, t_ms);
}

inline void SetCommandObserver(CommandObserver fn) {
  detail::CommandObserverState& state = detail::command_observer();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.fn = std::move(fn);
  state.present.store(bool(state.fn), std::memory_order_release);
}

// False means there is no consumer yet. The driver deliberately does not
// claim the event in that case, so an early poll during startup cannot lose a
// command before the title's command layer is installed.
inline bool NotifyCommand(size_t ordinal, const std::string& command, int64_t t_ms) {
  detail::CommandObserverState& state = detail::command_observer();
  if (!state.present.load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.fn) return false;
  state.fn(ordinal, command, t_ms);
  return true;
}

struct Effect {
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  uint16_t buttons = 0;   // OR'd while active
  int axis = -1;          // 0=lx 1=ly 2=rx 3=ry, -1 none
  int16_t axis_value = 0;
  int trigger = -1;       // 0=lt 1=rt, -1 none
  uint8_t trigger_value = 0;
};

namespace detail {

inline uint16_t ButtonMask(const std::string& name) {
  using namespace rex::input;
  if (name == "a") return X_INPUT_GAMEPAD_A;
  if (name == "b") return X_INPUT_GAMEPAD_B;
  if (name == "x") return X_INPUT_GAMEPAD_X;
  if (name == "y") return X_INPUT_GAMEPAD_Y;
  if (name == "start") return X_INPUT_GAMEPAD_START;
  if (name == "back") return X_INPUT_GAMEPAD_BACK;
  if (name == "lb") return X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (name == "rb") return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (name == "l3") return X_INPUT_GAMEPAD_LEFT_THUMB;
  if (name == "r3") return X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (name == "up") return X_INPUT_GAMEPAD_DPAD_UP;
  if (name == "down") return X_INPUT_GAMEPAD_DPAD_DOWN;
  if (name == "left") return X_INPUT_GAMEPAD_DPAD_LEFT;
  if (name == "right") return X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (name == "guide") return X_INPUT_GAMEPAD_GUIDE;
  return 0;
}

// Inverse of ButtonMask, for the recorder's human-readable "buttons" field.
// Bits with no name (the 360 pad leaves 0x0800 and 0x8?? unassigned) are
// dropped here and survive only in the authoritative "b" mask.
inline std::string ButtonNames(uint16_t mask) {
  static const std::pair<uint16_t, const char*> kNames[] = {
      {rex::input::X_INPUT_GAMEPAD_A, "a"},
      {rex::input::X_INPUT_GAMEPAD_B, "b"},
      {rex::input::X_INPUT_GAMEPAD_X, "x"},
      {rex::input::X_INPUT_GAMEPAD_Y, "y"},
      {rex::input::X_INPUT_GAMEPAD_START, "start"},
      {rex::input::X_INPUT_GAMEPAD_BACK, "back"},
      {rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER, "lb"},
      {rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER, "rb"},
      {rex::input::X_INPUT_GAMEPAD_LEFT_THUMB, "l3"},
      {rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB, "r3"},
      {rex::input::X_INPUT_GAMEPAD_DPAD_UP, "up"},
      {rex::input::X_INPUT_GAMEPAD_DPAD_DOWN, "down"},
      {rex::input::X_INPUT_GAMEPAD_DPAD_LEFT, "left"},
      {rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT, "right"},
      {rex::input::X_INPUT_GAMEPAD_GUIDE, "guide"},
  };
  std::string out;
  for (const auto& [bit, name] : kNames) {
    if (!(mask & bit)) continue;
    if (!out.empty()) out.push_back(' ');
    out += name;
  }
  return out;
}

// Minimal parser for exactly this schema: an array of flat objects whose
// values are double-quoted strings or integers. Unknown keys are ignored;
// malformed input logs and yields an empty timeline rather than crashing.
struct FlatObject {
  std::vector<std::pair<std::string, std::string>> fields;
  const std::string* find(const char* k) const {
    for (auto& [key, val] : fields)
      if (key == k) return &val;
    return nullptr;
  }
};

inline bool ParseFlatJson(const std::string& text, std::vector<FlatObject>& out,
                          std::string& err) {
  size_t i = 0;
  auto skip = [&] { while (i < text.size() && std::isspace(uint8_t(text[i]))) ++i; };
  auto fail = [&](const char* what) {
    std::ostringstream os;
    os << what << " at offset " << i;
    err = os.str();
    return false;
  };
  skip();
  if (i >= text.size() || text[i] != '[') return fail("expected '['");
  ++i; skip();
  if (i < text.size() && text[i] == ']') return true;
  while (true) {
    skip();
    if (i >= text.size() || text[i] != '{') return fail("expected '{'");
    ++i;
    FlatObject obj;
    while (true) {
      skip();
      if (i < text.size() && text[i] == '}') { ++i; break; }
      if (i >= text.size() || text[i] != '"') return fail("expected key quote");
      size_t k0 = ++i;
      while (i < text.size() && text[i] != '"') ++i;
      if (i >= text.size()) return fail("unterminated key");
      std::string key = text.substr(k0, i - k0);
      ++i; skip();
      if (i >= text.size() || text[i] != ':') return fail("expected ':'");
      ++i; skip();
      std::string value;
      if (i < text.size() && text[i] == '"') {
        size_t v0 = ++i;
        while (i < text.size() && text[i] != '"') ++i;
        if (i >= text.size()) return fail("unterminated string");
        value = text.substr(v0, i - v0);
        ++i;
      } else {
        size_t v0 = i;
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) ++i;
        while (i < text.size() && std::isdigit(uint8_t(text[i]))) ++i;
        if (i == v0) return fail("expected value");
        value = text.substr(v0, i - v0);
      }
      obj.fields.emplace_back(std::move(key), std::move(value));
      skip();
      if (i < text.size() && text[i] == ',') { ++i; continue; }
    }
    out.push_back(std::move(obj));
    skip();
    if (i < text.size() && text[i] == ',') { ++i; continue; }
    if (i < text.size() && text[i] == ']') return true;
    return fail("expected ',' or ']'");
  }
}

// Mark names end up inside a JSON string literal written by the recorder with
// snprintf and read back by a parser that has no escape handling, so the
// vocabulary is restricted at both ends rather than escaped: letters, digits,
// '_', '-' and '.'. Anything else becomes '_'. Shared with
// rexglue_input_record.h so a name cannot be written that cannot be read.
inline std::string SanitizeMarkName(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char c : raw) {
    out.push_back((std::isalnum(c) || c == '_' || c == '-' || c == '.')
                      ? char(c)
                      : '_');
  }
  if (out.empty()) out = "mark";
  if (out.size() > 64) out.resize(64);
  return out;
}

inline int64_t ToInt(const std::string* s, int64_t fallback) {
  if (!s) return fallback;
  try {
    return std::stoll(*s);
  } catch (...) {
    return fallback;
  }
}

inline std::vector<Effect> CompileTimeline(const std::string& text,
                                           std::vector<Mark>* marks,
                                           std::vector<Command>* commands) {
  std::vector<Effect> timeline;
  std::vector<FlatObject> objs;
  std::string err;
  if (!ParseFlatJson(text, objs, err)) {
    REXLOG_ERROR("script-input: bad --input_json ({}); ignoring script", err);
    return timeline;
  }
  int64_t cursor = 0;
  for (auto& obj : objs) {
    const std::string* action = obj.find("action");
    if (!action) continue;
    if (*action == "delay") {
      cursor += ToInt(obj.find("value"), 0);
      continue;
    }
    if (*action == "mark") {
      // Zero-duration: it names the cursor rather than occupying it, so a mark
      // between two entries does not shift the input that follows it.
      if (marks) {
        const std::string* at = obj.find("at");
        const std::string* value = obj.find("value");
        marks->push_back(Mark{at ? ToInt(at, cursor) : cursor,
                              SanitizeMarkName(value ? *value : "")});
      }
      continue;
    }
    if (*action == "command") {
      // Like a mark, a command names an instant rather than occupying one.
      if (commands) {
        const std::string* at = obj.find("at");
        const std::string* value = obj.find("value");
        if (!value || value->empty()) {
          REXLOG_WARN("script-input: ignoring empty command event");
        } else {
          commands->push_back(Command{at ? ToInt(at, cursor) : cursor, *value});
        }
      }
      continue;
    }
    Effect fx;
    int64_t hold = ToInt(obj.find("hold"), 150);
    const std::string* at = obj.find("at");
    fx.start_ms = at ? ToInt(at, cursor) : cursor;
    fx.end_ms = fx.start_ms + std::max<int64_t>(hold, 1);
    const std::string* value = obj.find("value");
    std::string v = value ? *value : "";
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (*action == "button") {
      fx.buttons = ButtonMask(v);
      if (!fx.buttons) {
        REXLOG_WARN("script-input: unknown button '{}'", v);
        continue;
      }
    } else if (*action == "axis") {
      fx.axis = v == "lx" ? 0 : v == "ly" ? 1 : v == "rx" ? 2 : v == "ry" ? 3 : -1;
      if (fx.axis < 0) continue;
      fx.axis_value = int16_t(std::clamp<int64_t>(
          ToInt(obj.find("position"), 0), -32768, 32767));
    } else if (*action == "trigger") {
      fx.trigger = v == "lt" ? 0 : v == "rt" ? 1 : -1;
      if (fx.trigger < 0) continue;
      fx.trigger_value = uint8_t(std::clamp<int64_t>(
          ToInt(obj.find("position"), 255), 0, 255));
    } else {
      REXLOG_WARN("script-input: unknown action '{}'", *action);
      continue;
    }
    timeline.push_back(fx);
    if (!at) cursor = fx.end_ms;
  }
  return timeline;
}

// The JSONL half. Reuses ParseFlatJson by wrapping each line in the array
// brackets it expects, so both formats go through one tested parser.
inline bool ParseJsonlLine(const std::string& line, FlatObject& out) {
  std::vector<FlatObject> objs;
  std::string err;
  if (!ParseFlatJson("[" + line + "]", objs, err) || objs.size() != 1) {
    return false;
  }
  out = std::move(objs[0]);
  return true;
}

inline std::vector<Sample> ParseJsonl(const std::string& text,
                                      std::vector<Mark>* marks,
                                      std::vector<Command>* commands) {
  std::vector<Sample> samples;
  std::istringstream in(text);
  std::string line;
  size_t lineno = 0, bad = 0;
  bool saw_header = false;
  while (std::getline(in, line)) {
    ++lineno;
    // Tolerate CRLF and blank separators.
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    FlatObject obj;
    if (!ParseJsonlLine(line, obj)) {
      // A truncated tail is the expected shape of a hard-killed recording:
      // note it and keep every complete line before it.
      ++bad;
      continue;
    }
    const std::string* type = obj.find("type");
    std::string t = type ? *type : "s";
    if (t == "header") {
      saw_header = true;
      int64_t version = ToInt(obj.find("version"), 1);
      if (version != 1) {
        REXLOG_WARN("script-input: recording format version {} is newer than this "
                    "build understands (1); reading it anyway",
                    version);
      }
      continue;
    }
    if (t == "end") continue;
    if (t == "mark") {
      if (marks) {
        const std::string* name = obj.find("name");
        marks->push_back(Mark{ToInt(obj.find("t"), 0),
                              SanitizeMarkName(name ? *name : "")});
      }
      continue;
    }
    if (t == "command") {
      if (commands) {
        const std::string* value = obj.find("value");
        if (!value) value = obj.find("command");
        if (!value || value->empty()) {
          REXLOG_WARN("script-input: ignoring empty command record on line {}", lineno);
        } else {
          commands->push_back(Command{ToInt(obj.find("t"), 0), *value});
        }
      }
      continue;
    }
    if (t != "s" && t != "sample") {
      REXLOG_WARN("script-input: unknown jsonl record type '{}' on line {}", t, lineno);
      continue;
    }
    Sample s;
    s.t_ms = ToInt(obj.find("t"), 0);
    const std::string* mask = obj.find("b");
    if (mask) {
      s.buttons = uint16_t(std::clamp<int64_t>(ToInt(mask, 0), 0, 0xFFFF));
    } else if (const std::string* names = obj.find("buttons")) {
      // Hand-edited line: names win when the machine mask was deleted.
      std::istringstream ns(*names);
      std::string name;
      while (ns >> name) {
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        uint16_t bit = ButtonMask(name);
        if (!bit) {
          REXLOG_WARN("script-input: unknown button '{}' on line {}", name, lineno);
          continue;
        }
        s.buttons |= bit;
      }
    }
    static const char* kAxisKeys[4] = {"lx", "ly", "rx", "ry"};
    for (int i = 0; i < 4; ++i) {
      s.axes[i] = int16_t(
          std::clamp<int64_t>(ToInt(obj.find(kAxisKeys[i]), 0), -32768, 32767));
    }
    s.triggers[0] = uint8_t(std::clamp<int64_t>(ToInt(obj.find("lt"), 0), 0, 255));
    s.triggers[1] = uint8_t(std::clamp<int64_t>(ToInt(obj.find("rt"), 0), 0, 255));
    samples.push_back(s);
  }
  if (!saw_header) {
    REXLOG_WARN("script-input: jsonl recording has no header line; assuming v1");
  }
  if (bad) {
    REXLOG_WARN("script-input: skipped {} malformed jsonl line(s) (a truncated "
                "tail is normal for a hard-killed recording)",
                bad);
  }
  std::stable_sort(samples.begin(), samples.end(),
                   [](const Sample& a, const Sample& b) { return a.t_ms < b.t_ms; });
  return samples;
}

}  // namespace detail

// What a loaded input file compiles to: an action timeline (the .json array
// format), a state track (the .jsonl recording format), or - if someone
// concatenates them - both, which merge exactly like two pads would.
struct Program {
  std::vector<Effect> timeline;
  std::vector<Sample> samples;
  std::vector<Mark> marks;
  std::vector<Command> commands;

  // Marks are annotation, not input: a file of nothing but marks is still
  // "no playable input" and says so, while the marks themselves still fire.
  bool empty() const { return timeline.empty() && samples.empty(); }
  int64_t end_ms() const {
    int64_t end = 0;
    for (const auto& fx : timeline) end = std::max(end, fx.end_ms);
    if (!samples.empty()) end = std::max(end, samples.back().t_ms);
    if (!marks.empty()) end = std::max(end, marks.back().t_ms);
    if (!commands.empty()) end = std::max(end, commands.back().t_ms);
    return end;
  }
};

// Sniffs the format from the first non-space character: '[' is the action
// array, anything else is treated as JSONL.
inline Program Compile(const std::string& text) {
  Program program;
  size_t i = 0;
  while (i < text.size() && std::isspace(uint8_t(text[i]))) ++i;
  if (i < text.size() && text[i] == '[') {
    program.timeline = detail::CompileTimeline(text, &program.marks,
                                               &program.commands);
  } else if (i < text.size()) {
    program.samples = detail::ParseJsonl(text, &program.marks,
                                         &program.commands);
  }
  // The driver fires marks by walking a cursor forward, so they have to be in
  // time order regardless of where they sat in the file.
  std::stable_sort(program.marks.begin(), program.marks.end(),
                   [](const Mark& a, const Mark& b) { return a.t_ms < b.t_ms; });
  std::stable_sort(program.commands.begin(), program.commands.end(),
                   [](const Command& a, const Command& b) {
                     return a.t_ms < b.t_ms;
                   });
  return program;
}

// What this process loaded and how far through it is. Published so a UI can
// read it without reaching into the driver, which lives inside the input
// system and is not addressable from the app.
//
// Everything except marks_fired is written once by Install() before the driver
// is constructed and never again, so the only synchronization needed is the
// `loaded` release/acquire pair that publishes it. marks_fired is bumped from
// guest polling threads and read from the UI thread; it is a display counter,
// so relaxed is right.
struct ReplayStatus {
  bool loaded = false;
  size_t actions = 0;
  size_t samples = 0;
  size_t marks_fired = 0;
  size_t commands_fired = 0;
  int64_t end_ms = 0;
  std::string source;
  const std::vector<Mark>* marks = nullptr;  // null unless loaded
  const std::vector<Command>* commands = nullptr;  // null unless loaded
};

namespace detail {

struct StatusState {
  std::atomic<bool> loaded{false};
  std::atomic<size_t> marks_fired{0};
  std::atomic<size_t> commands_fired{0};
  // Immutable once `loaded` is set.
  size_t actions = 0;
  size_t samples = 0;
  int64_t end_ms = 0;
  std::string source;
  std::vector<Mark> marks;
  std::vector<Command> commands;
};

inline StatusState& status() {
  static StatusState s;
  return s;
}

}  // namespace detail

inline ReplayStatus GetStatus() {
  detail::StatusState& s = detail::status();
  ReplayStatus out;
  if (!s.loaded.load(std::memory_order_acquire)) return out;
  out.loaded = true;
  out.actions = s.actions;
  out.samples = s.samples;
  out.end_ms = s.end_ms;
  out.source = s.source;
  out.marks = &s.marks;
  out.commands = &s.commands;
  out.marks_fired = s.marks_fired.load(std::memory_order_relaxed);
  out.commands_fired = s.commands_fired.load(std::memory_order_relaxed);
  return out;
}

class ScriptInputDriver : public rex::input::InputDriver {
 public:
  explicit ScriptInputDriver(Program program)
      : rex::input::InputDriver(nullptr, 0),
        program_(std::move(program)),
        t0_(Origin()) {}

  // Never called in practice: InputSystem::Setup() does not walk its drivers -
  // the SDK sets its own up before AddDriver. Install() logs what it loaded
  // instead, so a scripted or replayed run always leaves evidence in the log.
  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  rex::X_RESULT GetCapabilities(uint32_t user_index, uint32_t /*flags*/,
                           rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    if (user_index != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = int16_t(0xFFFFu);
      out_caps->gamepad.thumb_ly = int16_t(0xFFFFu);
      out_caps->gamepad.thumb_rx = int16_t(0xFFFFu);
      out_caps->gamepad.thumb_ry = int16_t(0xFFFFu);
    }
    return X_ERROR_SUCCESS;
  }

  // Monotonic count of pad-0 polls; the poll clock's time base.
  mutable std::atomic<uint64_t> polls_{0};
  // Handover point: the poll index and wall time at which the poll clock took
  // over. Zero means it has not engaged yet.
  mutable std::atomic<uint64_t> poll_base_{0};
  mutable std::atomic<int64_t> time_base_{0};

  rex::X_RESULT GetState(uint32_t user_index,
                    rex::input::X_INPUT_STATE* out_state) override {
    if (user_index != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out_state) return X_ERROR_SUCCESS;
    // The replay clock. Two of them, and which one runs decides whether a
    // measured run is reproducible.
    //
    // Wall clock (default): entries fire at host steady_clock times. A press
    // therefore lands on guest frame N or N+1 depending on sub-frame
    // alignment, and in this title a one-frame difference in an ollie ends the
    // run somewhere else entirely. Measured: two runs of the same fixture,
    // same binary, same flags, tracked to within 1% through t=15 s and then
    // diverged to a 2.6x difference in draw calls per frame by t=25 s, which
    // dragged effective fps from 59.9 to 41.3 and read exactly like a
    // performance regression.
    //
    // Poll clock (input_poll_clock=true): entries fire on a virtual clock that
    // advances one tick per GetState poll. The guest polls pad 0 exactly twice
    // per rendered frame in gameplay (measured: a flat 120 polls/s against
    // 60 fps, and a rate that tracks frames rather than wall time during
    // loading), so this pins every entry to a fixed guest frame. The same
    // fixture then applies the same input on the same frame regardless of how
    // fast the host ran.
    uint64_t poll_index = polls_.fetch_add(1, std::memory_order_relaxed) + 1;
    int64_t wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0_)
                       .count();
    int64_t now = wall;
    const bool poll_clock = REXCVAR_GET(input_poll_clock);
    if (poll_clock) {
      // Hand over from the wall clock rather than replacing it. Boot and menu
      // navigation are already reproducible - gameplay begins within 0.1 s of
      // the same moment in every run - and the poll rate is NOT 120/s during
      // loading (it tracks rendered frames, and a loading screen renders
      // fewer), so running the poll clock from t=0 stretches the menu inputs
      // and the fixture never reaches gameplay at all. Measured: it does not.
      //
      // So: wall clock until the handover point, then pin the offset and
      // advance purely on polls. Everything after the handover lands on a
      // fixed guest frame, which is the part that was diverging.
      int64_t after = REXCVAR_GET(input_poll_clock_after_ms);
      int64_t hz = std::max<int64_t>(1, REXCVAR_GET(input_poll_hz));
      uint64_t base_poll = poll_base_.load(std::memory_order_acquire);
      if (!base_poll) {
        if (wall >= after) {
          uint64_t expected = 0;
          if (poll_base_.compare_exchange_strong(expected, poll_index,
                                                 std::memory_order_acq_rel)) {
            time_base_.store(wall, std::memory_order_release);
            REXLOG_INFO("script-input: poll clock engaged at {} ms, poll {}", wall, poll_index);
          }
          base_poll = poll_base_.load(std::memory_order_acquire);
        }
      }
      if (base_poll) {
        int64_t elapsed = int64_t(((poll_index - base_poll) * 1000ull) / uint64_t(hz));
        now = time_base_.load(std::memory_order_acquire) + elapsed;
      }
    }
    {
      static std::atomic<int64_t> next_report{1000};
      int64_t due = next_report.load(std::memory_order_relaxed);
      if (now >= due &&
          next_report.compare_exchange_strong(due, now + 1000, std::memory_order_relaxed)) {
        static uint64_t last_n = 0;
        REXLOG_INFO("script-input: {} GetState polls in the last second ({} total, {} clock)",
                    poll_index - last_n, poll_index, poll_clock ? "poll" : "wall");
        last_n = poll_index;
      }
    }
    // Commands are posted before marks at the same timestamp. A fixture that
    // needs the guest-side effect to precede a screenshot should still leave a
    // few presented frames between the two: command callbacks may defer work
    // to the guest frame pump.
    FireDueCommands(now);
    FireDueMarks(now);
    uint16_t buttons = 0;
    int16_t axes[4] = {0, 0, 0, 0};
    uint8_t triggers[2] = {0, 0};
    for (const auto& fx : program_.timeline) {
      if (now < fx.start_ms || now >= fx.end_ms) continue;
      buttons |= fx.buttons;
      if (fx.axis >= 0 &&
          std::abs(int(fx.axis_value)) >= std::abs(int(axes[fx.axis]))) {
        axes[fx.axis] = fx.axis_value;
      }
      if (fx.trigger >= 0) {
        triggers[fx.trigger] = std::max(triggers[fx.trigger], fx.trigger_value);
      }
    }
    // A state track holds each sample until the next one. Binary search rather
    // than a cached cursor: GetState is polled from guest threads and a
    // stateless lookup needs no lock to stay correct. Before the first sample
    // the pad is neutral; after the last it holds - the recorder writes a
    // neutral sample at close so a complete recording ends with hands off.
    if (!program_.samples.empty()) {
      auto it = std::upper_bound(program_.samples.begin(), program_.samples.end(), now,
                                 [](int64_t t, const Sample& s) { return t < s.t_ms; });
      if (it != program_.samples.begin()) {
        const Sample& s = *(it - 1);
        buttons |= s.buttons;
        for (int i = 0; i < 4; ++i) {
          if (std::abs(int(s.axes[i])) >= std::abs(int(axes[i]))) axes[i] = s.axes[i];
        }
        triggers[0] = std::max(triggers[0], s.triggers[0]);
        triggers[1] = std::max(triggers[1], s.triggers[1]);
      }
    }
    std::memset(out_state, 0, sizeof(*out_state));
    // Coarse monotonic packet id; changes at least every 8 ms, which is finer
    // than any scripted transition worth observing.
    out_state->packet_number = uint32_t(now / 8 + 1);
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.thumb_lx = axes[0];
    out_state->gamepad.thumb_ly = axes[1];
    out_state->gamepad.thumb_rx = axes[2];
    out_state->gamepad.thumb_ry = axes[3];
    out_state->gamepad.left_trigger = triggers[0];
    out_state->gamepad.right_trigger = triggers[1];
    return X_ERROR_SUCCESS;
  }

  rex::X_RESULT SetState(uint32_t user_index,
                    rex::input::X_INPUT_VIBRATION* /*vibration*/) override {
    return user_index == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  rex::X_RESULT GetKeystroke(uint32_t user_index, uint32_t /*flags*/,
                        rex::input::X_INPUT_KEYSTROKE* /*out*/) override {
    return user_index == 0 ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  void FireDueCommands(int64_t now) {
    const auto& commands = program_.commands;
    size_t next = next_command_.load(std::memory_order_relaxed);
    while (next < commands.size() && now >= commands[next].t_ms) {
      // Do not claim an event until a consumer exists. OnPostSetup installs the
      // title observer; this closes the startup race without moving execution
      // onto the input thread permanently.
      if (!detail::command_observer().present.load(std::memory_order_acquire)) {
        bool expected = false;
        if (command_observer_warned_.compare_exchange_strong(
                expected, true, std::memory_order_relaxed)) {
          REXLOG_WARN("script-input: command due at {}ms but no command observer "
                      "is installed; keeping it pending", commands[next].t_ms);
        }
        return;
      }
      if (next_command_.compare_exchange_weak(next, next + 1,
                                              std::memory_order_relaxed)) {
        if (!NotifyCommand(next + 1, commands[next].text, commands[next].t_ms)) {
          // Observer removal raced the claim. This only happens during
          // shutdown; make the loss explicit rather than pretending it ran.
          REXLOG_WARN("script-input: command {} lost its observer during shutdown",
                      next + 1);
        } else {
          REXLOG_INFO("script-input: command {} t={}ms: {}", next + 1,
                      commands[next].t_ms, commands[next].text);
          detail::status().commands_fired.fetch_add(1,
                                                    std::memory_order_relaxed);
        }
        ++next;
      }
    }
  }

  // Logs every mark whose time has passed, exactly once each. GetState is
  // polled from several guest threads with no lock, so the cursor is claimed
  // with a CAS: whichever thread wins the slot is the one that logs it, and a
  // mark can therefore never be doubled or dropped. With no marks loaded this
  // is one relaxed load against a size and nothing else.
  void FireDueMarks(int64_t now) {
    const auto& marks = program_.marks;
    size_t next = next_mark_.load(std::memory_order_relaxed);
    while (next < marks.size() && now >= marks[next].t_ms) {
      // On success the CAS leaves `next` alone, so it still names the mark
      // this thread just claimed; on failure it reloads `next` and the loop
      // re-tests against the winner's progress.
      if (next_mark_.compare_exchange_weak(next, next + 1,
                                           std::memory_order_relaxed)) {
        REXLOG_INFO("script-input: mark '{}' t={}ms", marks[next].name,
                    marks[next].t_ms);
        detail::status().marks_fired.fetch_add(1, std::memory_order_relaxed);
        // Ordinal is the mark's position in the file, not a count of what has
        // fired, so the Nth mark of a fixture carries the same number in every
        // replay of it - which is what lets two runs' artefacts be paired.
        NotifyMark(next + 1, marks[next].name, marks[next].t_ms);
        ++next;
      }
    }
  }

  Program program_;
  std::chrono::steady_clock::time_point t0_;
  std::atomic<size_t> next_mark_{0};
  std::atomic<size_t> next_command_{0};
  std::atomic<bool> command_observer_warned_{false};
};

// A side-effect-only driver. InputSystem calls every driver and merges only
// successful states, so DEVICE_NOT_CONNECTED makes this invisible to the pad
// while still providing a known guest input thread for bounded observer work.
class PollObserverDriver : public rex::input::InputDriver {
 public:
  explicit PollObserverDriver(PollObserver observer)
      : rex::input::InputDriver(nullptr, 0), observer_(std::move(observer)) {}

  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  rex::X_RESULT GetCapabilities(
      uint32_t, uint32_t, rex::input::X_INPUT_CAPABILITIES*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  rex::X_RESULT GetState(
      uint32_t user_index, rex::input::X_INPUT_STATE*) override {
    if (user_index == 0 && observer_) observer_();
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  rex::X_RESULT SetState(
      uint32_t, rex::input::X_INPUT_VIBRATION*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  rex::X_RESULT GetKeystroke(
      uint32_t, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  PollObserver observer_;
};

// Reads --input_json / --input_json_file into raw text. Empty when neither is
// set. Shared with rexglue_input_record.h, which needs the same answer to
// decide whether a recording run also has a script to play.
inline std::string LoadScriptText() {
  std::string script = REXCVAR_QUERY(std::string, input_json);
  std::string script_file = REXCVAR_QUERY(std::string, input_json_file);
  if (script.empty() && !script_file.empty()) {
    std::ifstream in(script_file);
    if (in) {
      std::ostringstream buf;
      buf << in.rdbuf();
      script = buf.str();
    } else {
      REXLOG_ERROR("script-input: cannot read --input_json_file={}", script_file);
    }
  }
  return script;
}

// Wraps config.input_factory so the scripted driver is registered alongside
// the default drivers when --input_json/--input_json_file is set. An optional
// poll observer is appended after scripted input; without either feature this
// returns the original factory untouched.
inline void Install(rex::RuntimeConfig& config, PollObserver poll_observer = {}) {
  auto prev = config.input_factory;
  config.input_factory =
      [prev, poll_observer = std::move(poll_observer)](
          bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    // Pin the shared timebase before any driver exists, so a scripted timeline
    // and a recording taken in the same run agree on t=0.
    (void)Origin();
    std::string script = LoadScriptText();
    if (script.empty() && !poll_observer) {
      return prev ? prev(tool_mode)
                  : rex::input::CreateDefaultInputSystem(tool_mode);
    }
    // Build the concrete default system so AddDriver is reachable without
    // touching the SDK. If a custom factory was installed upstream we cannot
    // safely extend it; the script wins and says so.
    if (prev && !script.empty()) {
      REXLOG_WARN("script-input: replacing custom input_factory for scripted run");
    }
    auto system = rex::input::CreateDefaultInputSystem(tool_mode);
    if (!script.empty()) {
      Program program = Compile(script);
      std::string source = REXCVAR_QUERY(std::string, input_json_file);
      if (source.empty()) source = "--input_json";
      if (program.empty()) {
        REXLOG_ERROR("script-input: {} produced no playable input; slot 0 is idle",
                     source);
      } else {
        REXLOG_INFO("script-input: playing {} actions + {} recorded samples + {} "
                    "commands over {} ms on slot 0, from {}",
                    program.timeline.size(), program.samples.size(),
                    program.commands.size(), program.end_ms(), source);
      }
      // Listed up front as well as fired in place, so a run's phase boundaries
      // are readable from the log without waiting for the run to reach them.
      if (!program.marks.empty()) {
        std::string listed;
        for (const auto& mark : program.marks) {
          if (!listed.empty()) listed += ", ";
          listed += mark.name + "@" + std::to_string(mark.t_ms) + "ms";
        }
        REXLOG_INFO("script-input: {} mark(s): {}", program.marks.size(), listed);
      }
      {
        detail::StatusState& s = detail::status();
        s.actions = program.timeline.size();
        s.samples = program.samples.size();
        s.end_ms = program.end_ms();
        s.source = source;
        s.marks = program.marks;
        s.commands = program.commands;
        s.loaded.store(true, std::memory_order_release);
      }
      system->AddDriver(std::make_unique<ScriptInputDriver>(std::move(program)));
    }
    if (poll_observer) {
      system->AddDriver(std::make_unique<PollObserverDriver>(poll_observer));
    }
    return system;
  };
}

}  // namespace rexglue_script_input
