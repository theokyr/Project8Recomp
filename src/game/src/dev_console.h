// The developer-console command layer for this port.
//
// WHAT IS ALREADY THERE, AND WHAT THIS ADDS
//
// The SDK ships the console itself: `rex::ui::ConsoleDialog` (Backtick, bound
// in rex_app.cpp) gives us the log view, level/category filters, history and
// Tab autocomplete over the cvar registry. Registering a command is one macro,
// `REXCVAR_DEFINE_COMMAND_ARGS`, and because the registry is a process-wide
// singleton filled by static initialisers, anything registered in this project
// autocompletes, dispatches, and accepts `--name=value` on the command line
// with no SDK change at all. So this header does NOT reimplement a console.
//
// It adds the three things the game project cannot get from the registry:
//
//   1. A line executor with Source-engine semantics - `;` chaining, quoting,
//      `//` comments, and `wait` - shared by every entry point, so a line means
//      the same thing typed at the prompt, passed as `--exec=`, given as
//      `+command args`, or fired from a replay fixture. One grammar, four
//      doors.
//   2. Draining the `+command args` launch queue that SDK patch 0028 collects
//      in `rex::cvar::Init`, at a point where the app is actually up.
//   3. `dev_status`, which reports whether the machinery underneath actually
//      ran. See guest_call.h for why that is not optional here.
//
// ON DUPLICATING THE SPLITTER
//
// Patch 0028 teaches `ConsoleDialog::ExecuteCommand` the same `;` grammar. That
// code lives across a shared-library boundary and is private, so this executor
// cannot call it and instead matches it. The two are kept deliberately simple
// and identical in behaviour; `dev_console_selftest` exercises this side.
//
// COST WHEN OFF
//
// `--dev_console` defaults false. With it off, nothing is installed, no guest
// function is hooked, no launch command runs, and a run is byte-identical to
// one built without this header - the same rule guest_spin_yield.h states and
// the reason perf gates stay valid across this change.

#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "guest_call.h"

REXCVAR_DEFINE_BOOL(dev_console, false, "Dev",
                    "Enable the developer console command layer: +command launch "
                    "arguments, --exec, and the cheat surface. Off by default; a run "
                    "without it is unchanged.");

REXCVAR_DEFINE_STRING(exec, "", "Dev",
                      "Console line to run at startup, ';'-separated. "
                      "Example: --exec=\"cheat_unlock_all 1; cheat_warp 100 0 250\"");

REXCVAR_DEFINE_STRING(exec_file, "", "Dev",
                      "File of console lines to run at startup, one per line; "
                      "'//' begins a comment.");

namespace thps::dev_console {

namespace detail {

struct State {
  std::mutex mutex;
  std::map<std::string, std::string> aliases;
  uint64_t lines = 0;
  uint64_t statements = 0;
  uint64_t unknown = 0;
  bool launch_done = false;
};

inline State& state() {
  static State s;
  return s;
}

inline std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.remove_suffix(1);
  return s;
}

// Strips a `//` comment that is not inside double quotes. Kept separate from
// splitting so a fixture command and an exec-file line behave identically.
inline std::string_view StripComment(std::string_view s) {
  bool quoted = false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) { ++i; continue; }
    if (s[i] == '"') { quoted = !quoted; continue; }
    if (!quoted && s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
      return s.substr(0, i);
  }
  return s;
}

// Splits on ';' at quote depth zero. Must match patch 0028's
// ConsoleDialog::SplitStatements.
inline std::vector<std::string_view> Split(std::string_view line) {
  std::vector<std::string_view> out;
  bool quoted = false;
  size_t start = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size()) { ++i; continue; }
    if (line[i] == '"') { quoted = !quoted; continue; }
    if (line[i] == ';' && !quoted) {
      out.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  out.push_back(line.substr(start));
  return out;
}

// Unquotes a whole argument string: `"a b"` -> `a b`. Only strips a matched
// outer pair, so `--flag="x"` style values survive.
inline std::string Unquote(std::string_view s) {
  s = Trim(s);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    s = s.substr(1, s.size() - 2);
  return std::string(s);
}

}  // namespace detail

// Splits a statement into `name` and the raw argument text after it.
inline void SplitStatement(std::string_view stmt, std::string& name, std::string& args) {
  stmt = detail::Trim(stmt);
  size_t sep = stmt.find(' ');
  if (sep == std::string_view::npos) {
    name = std::string(stmt);
    args.clear();
    return;
  }
  name = std::string(stmt.substr(0, sep));
  args = std::string(detail::Trim(stmt.substr(sep + 1)));
}

void ExecuteLine(std::string_view line);

namespace detail {

// Runs `stmts[from..]` once `guest_call::frames()` has advanced by `frames`.
// Re-posts itself rather than sleeping, so nothing blocks a guest thread.
inline void ScheduleRemainder(std::vector<std::string> stmts, size_t from,
                              uint64_t deadline) {
  guest_call::Post([stmts = std::move(stmts), from, deadline]() mutable {
    if (guest_call::frames() < deadline) {
      ScheduleRemainder(std::move(stmts), from, deadline);
      return;
    }
    std::string rest;
    for (size_t i = from; i < stmts.size(); ++i) {
      if (!rest.empty()) rest += ';';
      rest += stmts[i];
    }
    ExecuteLine(rest);
  });
}

// One already-split statement. Mirrors the SDK console's precedence: command
// dispatch first, then cvar get (no args) or set (args).
inline void ExecuteOne(std::string_view stmt) {
  stmt = Trim(stmt);
  if (stmt.empty()) return;
  State& s = state();
  s.statements++;

  std::string name, args;
  SplitStatement(stmt, name, args);

  // Alias expansion, one level - enough for convenience, and it cannot loop.
  // The map lookup copies out under the lock and the recursive ExecuteLine runs
  // after it is released, because an alias body may itself set a cvar or define
  // another alias.
  std::string expanded;
  {
    std::lock_guard lock(s.mutex);
    auto it = s.aliases.find(name);
    if (it != s.aliases.end()) {
      expanded = it->second;
      if (!args.empty()) expanded += " " + args;
    }
  }
  if (!expanded.empty()) {
    ExecuteLine(expanded);
    return;
  }

  const auto* info = rex::cvar::GetFlagInfo(name);
  if (info && info->type == rex::cvar::FlagType::Command) {
    REXLOG_INFO("[dev] > {}{}{}", name, args.empty() ? "" : " ", args);
    rex::cvar::InvokeCommand(name, args);
    return;
  }
  if (args.empty()) {
    std::string value = rex::cvar::GetFlagByName(name);
    if (value.empty() && !info) {
      s.unknown++;
      REXLOG_WARN("[dev] unknown command or cvar: {}", name);
    } else {
      REXLOG_INFO("[dev] {} = {}", name, value);
    }
    return;
  }
  if (rex::cvar::SetFlagByName(name, Unquote(args))) {
    REXLOG_INFO("[dev] {} = {}", name, args);
  } else {
    s.unknown++;
    REXLOG_WARN("[dev] unknown cvar, or value rejected: {} = {}", name, args);
  }
}

}  // namespace detail

// The one entry point. `;`-chained, quote-aware, `//`-commented, and `wait N`
// defers everything after it by N presented guest frames.
inline void ExecuteLine(std::string_view line) {
  line = detail::StripComment(line);
  line = detail::Trim(line);
  if (line.empty()) return;
  detail::state().lines++;

  auto parts = detail::Split(line);
  for (size_t i = 0; i < parts.size(); ++i) {
    std::string_view stmt = detail::Trim(parts[i]);
    if (stmt.empty()) continue;

    std::string name, args;
    SplitStatement(stmt, name, args);
    if (name == "wait") {
      uint64_t n = 1;
      if (!args.empty()) {
        try { n = std::stoull(args); } catch (...) { n = 1; }
      }
      if (i + 1 >= parts.size()) return;  // nothing left to defer
      if (!guest_call::installed()) {
        REXLOG_WARN("[dev] wait: no guest frame pump; running the rest immediately");
      } else {
        std::vector<std::string> rest;
        for (size_t j = i + 1; j < parts.size(); ++j) rest.emplace_back(parts[j]);
        detail::ScheduleRemainder(std::move(rest), 0, guest_call::frames() + n);
        return;
      }
      continue;
    }
    detail::ExecuteOne(stmt);
  }
}

inline void ExecuteFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    REXLOG_WARN("[dev] exec: cannot open {}", path);
    return;
  }
  std::string line;
  while (std::getline(in, line)) ExecuteLine(line);
}

// Drains the `+command args` queue that SDK patch 0028 collected in
// rex::cvar::Init. Safe to call when the SDK is unpatched: the symbol is
// resolved at link time against the prefix the build was configured with, and
// an unpatched prefix simply has no launch commands to hand back.
inline void RunLaunchCommands() {
  detail::State& s = detail::state();
  if (s.launch_done) return;
  s.launch_done = true;
  auto commands = rex::cvar::TakeLaunchCommands();
  if (!commands.empty())
    REXLOG_INFO("[dev] {} launch command(s) from +arguments", commands.size());
  for (const auto& c : commands) ExecuteLine(c);
}

inline void RunStartupExec() {
  const std::string& file = REXCVAR_GET(exec_file);
  if (!file.empty()) ExecuteFile(file);
  const std::string& line = REXCVAR_GET(exec);
  if (!line.empty()) ExecuteLine(line);
}

inline uint64_t lines() { return detail::state().lines; }
inline uint64_t statements() { return detail::state().statements; }
inline uint64_t unknown() { return detail::state().unknown; }

}  // namespace thps::dev_console

REXCVAR_DEFINE_COMMAND_ARGS(
    exec_line,
    [](std::string_view args) { thps::dev_console::ExecuteLine(args); },
    "Dev", "Run a ';'-chained console line through the project executor");

REXCVAR_DEFINE_COMMAND_ARGS(
    exec_script,
    ([](std::string_view args) {
      thps::dev_console::ExecuteFile(std::string(thps::dev_console::detail::Trim(args)));
    }),
    "Dev", "Run a file of console lines (one per line, '//' comments)");

REXCVAR_DEFINE_COMMAND_ARGS(
    alias,
    ([](std::string_view args) {
      std::string name, body;
      thps::dev_console::SplitStatement(args, name, body);
      auto& s = thps::dev_console::detail::state();
      if (name.empty()) {
        std::lock_guard lock(s.mutex);
        for (const auto& [k, v] : s.aliases) REXLOG_INFO("[dev] alias {} = {}", k, v);
        return;
      }
      std::lock_guard lock(s.mutex);
      if (body.empty()) {
        s.aliases.erase(name);
        REXLOG_INFO("[dev] alias {} cleared", name);
      } else {
        s.aliases[name] = thps::dev_console::detail::Unquote(body);
        REXLOG_INFO("[dev] alias {} = {}", name, s.aliases[name]);
      }
    }),
    "Dev", "alias <name> \"<line>\" - define, or with no args list, aliases");

REXCVAR_DEFINE_COMMAND(
    dev_status,
    ([]() {
      // Deliberately reports the *mechanism*, not just the outcome. A cheat
      // that silently no-ops and a cheat that had no visible effect look
      // identical in a screenshot or a frame time; these counters separate
      // them.
      REXLOG_INFO("[dev] console: {} lines, {} statements, {} unknown",
                  thps::dev_console::lines(), thps::dev_console::statements(),
                  thps::dev_console::unknown());
      REXLOG_INFO("[dev] guest pump: installed={} frames={} posted={} drained={} "
                  "threadless={}",
                  thps::guest_call::installed(), thps::guest_call::frames(),
                  thps::guest_call::posted(), thps::guest_call::drained(),
                  thps::guest_call::threadless_pumps());
    }),
    "Dev", "Report console and guest-call-pump counters");

REXCVAR_DEFINE_COMMAND(
    dev_ping,
    ([]() {
      // The end-to-end proof that a console command can reach a guest thread.
      thps::guest_call::Post([]() {
        REXLOG_INFO("[dev] ping ran on a guest thread at frame {}",
                    thps::guest_call::frames());
      });
      REXLOG_INFO("[dev] ping posted; expect one guest-thread line next frame");
    }),
    "Dev", "Post a no-op job to the guest frame pump and log when it runs");
