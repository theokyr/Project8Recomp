// Cheats, built on the engine's own script-command table.
//
// The table (guest_probe.h) hands us 1,366 named guest functions. Calling one
// is the cheat mechanism: `ChangeLevel` is a level warp, `RestartLevel` is a
// respawn, `UnlockSkater` is an unlock. Nothing here pokes bytes at an address
// found by trial and error when the engine already exports a function that
// does the job under a name it chose itself.
//
// TWO CONSTRAINTS SHAPE EVERYTHING BELOW
//
// 1. A script command's C++ signature is `handler(CStruct* params, CScript*
//    script)`. Passing 0 is not a way around `params`: sub_82212218 opens with
//    `lwz r30,4(r3)` and no null check. The retail binary does, however, retain
//    the complete CStruct lifecycle and typed adders. GuestStruct below uses
//    its constructor (0x82211B90), destructor (0x82213B18), AddString
//    (0x82214768), and AddChecksum (0x82214A50) against a 16-byte object on the
//    current guest stack. This is the game's allocator and representation, not
//    a host-side guess. The original 109-command no-parameter allowlist remains
//    the gate for generic `script_call Name` invocations.
//
// 2. Calls must happen on a guest thread. `GuestToHostFunction` returns a
//    value-initialised result and does nothing at all when
//    `ThreadState::Get()` is null, which is exactly the case on the UI thread
//    where console commands run. Everything here therefore goes through
//    `guest_call::Post`, and every call increments a counter so `cheat_status`
//    can distinguish "ran and did nothing" from "never ran".

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/stack.h>
#include <rex/runtime.h>

#include "guest_call.h"
#include "guest_probe.h"

REXCVAR_DEFINE_BOOL(cheat_unsafe_calls, false, "Cheats",
                    "Allow script_call to invoke commands that read a parameter "
                    "struct with a null one. The engine's CStruct getter does not "
                    "null-check, so this can fault. Debugging only.");

namespace thps::cheats {

namespace detail {

struct State {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> call_failures{0};
  std::atomic<uint64_t> structs_built{0};
  std::atomic<uint64_t> script_spawns{0};
  std::atomic<uint64_t> teleports{0};
  std::atomic<uint64_t> warps{0};
  std::atomic<uint64_t> zone_changes{0};
  std::atomic<bool> ready{false};
};

inline State& state() {
  static State s;
  return s;
}

}  // namespace detail

// Resolves a guest function address to something callable. The dispatcher is
// the same table the recompiler filled, so a hit here also proves the address
// belongs to this dump.
inline ::PPCFunc* Resolve(uint32_t addr) {
  auto* runtime = rex::Runtime::instance();
  if (!runtime) return nullptr;
  auto* dispatcher = runtime->function_dispatcher();
  if (!dispatcher) return nullptr;
  return dispatcher->GetFunction(addr);
}

inline constexpr uint32_t kCStructConstruct = 0x82211B90;
inline constexpr uint32_t kCStructDestroy = 0x82213B18;
inline constexpr uint32_t kCStructAddString = 0x82214768;
inline constexpr uint32_t kCStructAddChecksum = 0x82214A50;
inline constexpr uint32_t kCStructAddVector = 0x82214AD8;
// The retail asynchronous script constructor. Shipped callers use the minimal
// form QueueScript(script checksum, params, 0, 0, 0, 0, 0, 0, false, false).
// The nearby 0x822109A0 runner is synchronous: a script that waits for a later
// game tick will pin the calling guest thread inside it.
inline constexpr uint32_t kQueueScript = 0x82210860;
// The retail QB global-symbol hash table pointer. sub_822104B0 and
// sub_82211BE0 establish the node contract: type at +2, checksum at +4,
// value at +12 and the bucket chain at +16. Type 13 aliases through the
// checksum in value; type 10 stores a CStruct pointer there.
inline constexpr uint32_t kGlobalSymbolTablePointer = 0x8274A4B4;
inline constexpr uint8_t kSymbolStructure = 10;
inline constexpr uint8_t kSymbolName = 13;
inline constexpr uint32_t kGetLocalSkater = 0x822DF688;
inline constexpr uint32_t kSkaterManagerGlobal = 0x8276F3DC;
inline constexpr uint32_t kSkaterPositionOffset = 0x70;

template <typename... Args>
inline bool InvokeVoid(uint32_t addr, Args... args) {
  ::PPCFunc* fn = Resolve(addr);
  if (!fn) {
    REXLOG_WARN("[cheat] helper {:08X} is not a registered guest function", addr);
    detail::state().call_failures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  GuestToHostFunction<void>(fn, args...);
  return true;
}

// A real retail CStruct whose storage lives for exactly one pumped job. The
// typed adders allocate components on the guest heap and the destructor frees
// them before stack_guard restores r1. No guest pointer escapes the scope.
class GuestStruct {
 public:
  GuestStruct() : guard_() {
    std::array<uint8_t, 16> zero{};
    address_ = rex::ppc::stack_push(zero.data(), zero.size());
    valid_ = InvokeVoid(kCStructConstruct, address_);
    if (valid_) detail::state().structs_built.fetch_add(1, std::memory_order_relaxed);
  }

  ~GuestStruct() {
    if (valid_) InvokeVoid(kCStructDestroy, address_);
  }

  GuestStruct(const GuestStruct&) = delete;
  GuestStruct& operator=(const GuestStruct&) = delete;

  bool valid() const { return valid_; }
  uint32_t address() const { return address_; }

  bool AddString(uint32_t key, const std::string& value) {
    if (!valid_) return false;
    const uint32_t guest_string = rex::ppc::stack_push_string(value.c_str());
    return InvokeVoid(kCStructAddString, address_, key, guest_string);
  }

  bool AddChecksum(uint32_t key, uint32_t value) {
    return valid_ && InvokeVoid(kCStructAddChecksum, address_, key, value);
  }

  bool AddVector(uint32_t key, float x, float y, float z) {
    return valid_ && InvokeVoid(kCStructAddVector, address_, key, x, y, z);
  }

 private:
  rex::ppc::stack_guard guard_;
  uint32_t address_ = 0;
  bool valid_ = false;
};

inline uint32_t SpawnScriptOnGuestThread(std::string_view name,
                                         uint32_t params = 0) {
  ::PPCFunc* spawn = Resolve(kQueueScript);
  if (!spawn) {
    REXLOG_WARN("[cheat] script-queue helper {:08X} is unavailable",
                kQueueScript);
    detail::state().call_failures.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const uint32_t checksum = probe::Checksum(name);
  // sub_82210860 preserves the retail script-launch ABI: r3 is the script
  // checksum and r4 is the CStruct parameter source. Its wrapper moves that
  // original r4 into sub_8220EBC8's r6, where the constructor clones it into
  // the queued script object before returning. A temporary GuestStruct is
  // therefore valid for asynchronous launches.
  const uint32_t result = GuestToHostFunction<uint32_t>(
      spawn, checksum, params, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
  detail::state().script_spawns.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("[cheat] script '{}' ({:08X}) returned {:08X}", name, checksum,
              result);
  return result;
}

struct GlobalSymbol {
  uint32_t address = 0;
  uint32_t checksum = 0;
  uint32_t value = 0;
  uint8_t type = 0;
};

// Mirrors the bounded lookup in the retail script VM. The two limits turn a
// corrupt bucket or alias loop into a visible refusal rather than an endless
// guest-frame job.
inline bool ResolveGlobalSymbol(uint32_t checksum, GlobalSymbol& out) {
  uint32_t table = 0;
  if (!probe::ReadU32(kGlobalSymbolTablePointer, table) || !table) {
    REXLOG_WARN("[cheat] QB global-symbol table is unavailable");
    return false;
  }

  for (uint32_t aliases = 0; aliases < 8; ++aliases) {
    const uint32_t bucket = table + ((checksum << 2) & 0x1FFFCu);
    uint32_t node = 0;
    if (!probe::ReadU32(bucket, node)) return false;

    bool found = false;
    for (uint32_t links = 0; node && links < 4096; ++links) {
      uint32_t candidate = 0;
      if (!probe::ReadU32(node + 4, candidate)) return false;
      if (candidate == checksum) {
        found = true;
        break;
      }
      if (!probe::ReadU32(node + 16, node)) return false;
    }
    if (!found) return false;

    uint8_t type = 0;
    uint32_t value = 0;
    if (!probe::ReadU8(node + 2, type) ||
        !probe::ReadU32(node + 12, value)) return false;
    if (type != kSymbolName) {
      out = {.address = node, .checksum = checksum,
             .value = value, .type = type};
      return true;
    }
    checksum = value;
  }
  REXLOG_WARN("[cheat] QB global alias chain exceeded its safety bound");
  return false;
}

inline std::string CleanArgument(std::string_view raw) {
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front())))
    raw.remove_prefix(1);
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back())))
    raw.remove_suffix(1);
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
    raw = raw.substr(1, raw.size() - 2);
  return std::string(raw);
}

// Commands this module is willing to call with (0, 0).
//
// Each verdict is the `params`/`script` result from the script-command
// classifier used when this allowlist was authored,
// and "unused" means the register is written before it is ever read and never
// live across a call, so a 0 cannot be dereferenced by this function or
// anything it calls. The list is by NAME rather than address so it survives a
// table that moves; the address is resolved from the live table at call time.
//
// Kept explicit rather than generated: this is the allowlist that stops a
// typo at the console from calling something that walks a null CStruct, and it
// should be read and extended deliberately.
inline bool IsCallableWithoutParams(std::string_view name) {
  static constexpr std::string_view kSafe[] = {
      // Frontend / movie state - the intro-skip gating predicates.
      "InFrontend", "IsMoviePlaying", "IsMovieQueued", "HasMovieStarted",
      // Mode predicates.
      "IsCareerMode", "InMultiplayerGame", "InNetGame", "InTeamGame", "IsOnline",
      // Level and game launch.
      "ChangeLevelPending", "LaunchLevel", "LaunchGame",
      // Pause / resume that take nothing.
      "UnPauseGame", "UnPauseObjects", "PauseObjects",
      // The engine's own diagnostics - a memory and render-metrics readout the
      // retail build still carries.
      "DumpMemStatistics", "DumpHavokMemStats", "DumpCOIMEntries",
      "DumpNetMessageStats", "ToggleRenderMetrics", "RenderingEnabled",
      // Skater bookkeeping.
      "InitializeSkaters", "ReinsertSkaters", "AllSkatersAreIdle",
      "ResetComboRecords", "ClearPowerups", "ResetProSetFlags",
  };
  for (std::string_view s : kSafe)
    if (s.size() == name.size() &&
        std::equal(s.begin(), s.end(), name.begin(),
                   [](char a, char b) {
                     return ::tolower(static_cast<unsigned char>(a)) ==
                            ::tolower(static_cast<unsigned char>(b));
                   }))
      return true;
  return false;
}

// Synchronous half of CallByName. This is also what a GuestStruct-backed job
// uses: its stack address must be consumed before that job returns.
inline bool CallByNameOnGuestThread(std::string_view name, uint32_t params,
                                    uint32_t script, uint32_t& result) {
  if (probe::commands().empty()) probe::ScanScriptTable();
  const uint32_t addr = probe::LookupCaseless(name);
  if (!addr) {
    REXLOG_WARN("[cheat] no script command named '{}'", name);
    return false;
  }
  if (probe::IsStub(addr)) {
    REXLOG_WARN("[cheat] '{}' is a retail no-op stub ({:08X}); calling it would "
                "do nothing and look like a working cheat", name, addr);
    return false;
  }
  // The allowlist only governs the no-parameter call. Once a caller supplies a
  // real params/script pointer it is answering for them itself.
  if (params == 0 && script == 0 && !IsCallableWithoutParams(name) &&
      !REXCVAR_GET(cheat_unsafe_calls)) {
    REXLOG_WARN("[cheat] '{}' is not proven safe with null parameters; generic "
                "script_call cannot infer its schema. See "
                "`script_commands.py --classify`. Override with "
                "--cheat_unsafe_calls=true if you are debugging and accept a fault.",
                name);
    return false;
  }
  ::PPCFunc* fn = Resolve(addr);
  if (!fn) {
    REXLOG_WARN("[cheat] {} at {:08X} is not a registered guest function", name, addr);
    detail::state().call_failures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  result = GuestToHostFunction<uint32_t>(fn, params, script);
  detail::state().calls.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Calls a script command by name on the next guest frame.
//
// `params` and `script` are persistent guest pointers passed as r3/r4. The
// ordinary console form uses 0/0 and is guarded by the proven allowlist above;
// temporary GuestStruct pointers use CallByNameOnGuestThread directly.
inline bool CallByName(std::string_view name, uint32_t params = 0,
                       uint32_t script = 0) {
  // Validate before posting so a typo gets immediate feedback at the console.
  if (probe::commands().empty()) probe::ScanScriptTable();
  if (!probe::LookupCaseless(name)) {
    REXLOG_WARN("[cheat] no script command named '{}'", name);
    return false;
  }
  const std::string owned(name);
  guest_call::Post([owned, params, script]() {
    uint32_t result = 0;
    if (CallByNameOnGuestThread(owned, params, script, result))
      REXLOG_INFO("[cheat] {} returned {}", owned, result);
  });
  return true;
}

inline void Install() {
  const size_t n = probe::ScanScriptTable();
  if (n == 0) {
    REXLOG_WARN("[cheat] script-command table not readable yet; `script_table` "
                "can rescan once the guest image is mapped");
    return;
  }
  detail::state().ready.store(true, std::memory_order_relaxed);
  REXLOG_INFO("[cheat] {} script commands available (try `script_find level`)", n);
}

inline uint64_t calls() {
  return detail::state().calls.load(std::memory_order_relaxed);
}
inline uint64_t call_failures() {
  return detail::state().call_failures.load(std::memory_order_relaxed);
}
inline uint64_t structs_built() {
  return detail::state().structs_built.load(std::memory_order_relaxed);
}
inline uint64_t script_spawns() {
  return detail::state().script_spawns.load(std::memory_order_relaxed);
}
inline uint64_t teleports() {
  return detail::state().teleports.load(std::memory_order_relaxed);
}
inline uint64_t warps() {
  return detail::state().warps.load(std::memory_order_relaxed);
}
inline uint64_t zone_changes() {
  return detail::state().zone_changes.load(std::memory_order_relaxed);
}

}  // namespace thps::cheats

REXCVAR_DEFINE_COMMAND_ARGS(
    script_call,
    ([](std::string_view args) {
      std::string name(args);
      uint32_t params = 0, script = 0;
      char namebuf[128] = {};
      if (std::sscanf(name.c_str(), "%127s %x %x", namebuf, &params, &script) >= 1)
        thps::cheats::CallByName(namebuf, params, script);
      else
        REXLOG_WARN("[cheat] usage: script_call <Name> [hex-params] [hex-script]");
    }),
    "Cheats",
    "script_call <Name> [params] [script] - call an engine script command "
    "(developer tool: a command that needs parameters may fault without them)");

REXCVAR_DEFINE_COMMAND(
    cheat_status,
    ([]() {
      // Provenance, so a bench artifact can never quietly compare a cheated arm
      // against a clean one.
      REXLOG_INFO("[cheat] table entries={} script calls={} failures={} "
                  "structs={} script_spawns={} teleports={} warps={} zones={}",
                  thps::probe::commands().size(), thps::cheats::calls(),
                  thps::cheats::call_failures(), thps::cheats::structs_built(),
                  thps::cheats::script_spawns(),
                  thps::cheats::teleports(), thps::cheats::warps(),
                  thps::cheats::zone_changes());
    }),
    "Cheats", "Report which cheat machinery has actually run this session");

// A synchronous no-parameter call, for predicates whose answer we need now.
// Must run on a guest thread, so this is only valid from inside a pumped job.
namespace thps::cheats {

inline bool CallPredicateOnGuestThread(std::string_view name, uint32_t& out) {
  const uint32_t addr = probe::LookupCaseless(name);
  if (!addr || probe::IsStub(addr) || !IsCallableWithoutParams(name)) return false;
  ::PPCFunc* fn = Resolve(addr);
  if (!fn) return false;
  out = GuestToHostFunction<uint32_t>(fn, 0u, 0u);
  return true;
}

struct Position {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 0.0f;
};

inline bool GetLocalSkaterOnGuestThread(uint32_t& skater) {
  uint32_t manager = 0;
  if (!probe::ReadU32(kSkaterManagerGlobal, manager) || !manager) {
    REXLOG_WARN("[cheat] local-skater manager is not available yet");
    return false;
  }
  ::PPCFunc* fn = Resolve(kGetLocalSkater);
  if (!fn) {
    REXLOG_WARN("[cheat] local-skater resolver {:08X} is unavailable",
                kGetLocalSkater);
    detail::state().call_failures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  skater = GuestToHostFunction<uint32_t>(fn, manager);
  if (!skater) {
    REXLOG_WARN("[cheat] no local skater exists in the current game state");
    return false;
  }
  return true;
}

inline bool ReadPosition(uint32_t skater, Position& p) {
  const uint32_t va = skater + kSkaterPositionOffset;
  return probe::ReadFloat(va + 0, p.x) && probe::ReadFloat(va + 4, p.y) &&
         probe::ReadFloat(va + 8, p.z) && probe::ReadFloat(va + 12, p.w);
}

inline bool WritePosition(uint32_t skater, const Position& p) {
  const uint32_t va = skater + kSkaterPositionOffset;
  // Resolve the whole vector before the first write so failure cannot leave a
  // partially-updated position.
  if (!probe::Resolve(va) || !probe::Resolve(va + 15)) return false;
  return probe::WriteFloat(va + 0, p.x) && probe::WriteFloat(va + 4, p.y) &&
         probe::WriteFloat(va + 8, p.z) && probe::WriteFloat(va + 12, p.w);
}

}  // namespace thps::cheats

REXCVAR_DEFINE_COMMAND_ARGS(
    cheat_try_code,
    ([](std::string_view args) {
      std::string code = thps::cheats::CleanArgument(args);
      if (code.empty()) {
        REXLOG_WARN("[cheat] usage: cheat_try_code <retail cheat text>");
        return;
      }
      thps::guest_call::Post([code = std::move(code)]() {
        thps::cheats::GuestStruct params;
        // Shipped dbg.pak.xen: 0x61414D56 -> "string". TryCheatString asks for
        // this key through the string getter at 0x82212780.
        if (!params.valid() || !params.AddString(0x61414D56, code)) {
          REXLOG_WARN("[cheat] could not build TryCheatString parameters");
          return;
        }
        uint32_t result = 0;
        if (thps::cheats::CallByNameOnGuestThread(
                "TryCheatString", params.address(), 0, result)) {
          REXLOG_INFO("[cheat] TryCheatString returned {} for '{}'", result, code);
        }
      });
    }),
    "Cheats", "cheat_try_code <text> - submit text to the retail cheat parser");

REXCVAR_DEFINE_COMMAND_ARGS(
    cheat_change_level,
    ([](std::string_view args) {
      std::string level = thps::cheats::CleanArgument(args);
      if (level.empty()) {
        REXLOG_WARN("[cheat] usage: cheat_change_level <level-name>");
        return;
      }
      thps::guest_call::Post([level = std::move(level)]() {
        thps::cheats::GuestStruct params;
        // Shipped dbg.pak.xen: 0x651533EC -> "level". ChangeLevel consumes it
        // with the name/checksum getter at 0x82212D48.
        if (!params.valid() ||
            !params.AddChecksum(0x651533EC, thps::probe::Checksum(level))) {
          REXLOG_WARN("[cheat] could not build ChangeLevel parameters");
          return;
        }
        uint32_t result = 0;
        if (thps::cheats::CallByNameOnGuestThread(
                "ChangeLevel", params.address(), 0, result)) {
          REXLOG_INFO("[cheat] ChangeLevel('{}', {:08X}) returned {}", level,
                      thps::probe::Checksum(level), result);
        }
      });
    }),
    "Cheats", "cheat_change_level <name> - load a level through the retail command");

REXCVAR_DEFINE_COMMAND(
    cheat_position,
    ([]() {
      thps::guest_call::Post([]() {
        uint32_t skater = 0;
        thps::cheats::Position p;
        if (!thps::cheats::GetLocalSkaterOnGuestThread(skater) ||
            !thps::cheats::ReadPosition(skater, p)) {
          REXLOG_WARN("[cheat] could not read the local skater position");
          return;
        }
        REXLOG_INFO("[cheat] skater {:08X} position {:.3f} {:.3f} {:.3f} "
                    "(w={:.3f})", skater, p.x, p.y, p.z, p.w);
      });
    }),
    "Cheats", "Report the local skater's world position");

REXCVAR_DEFINE_COMMAND_ARGS(
    cheat_teleport,
    ([](std::string_view args) {
      thps::cheats::Position target;
      char extra = 0;
      const int parsed = std::sscanf(std::string(args).c_str(), "%f %f %f %c",
                                     &target.x, &target.y, &target.z, &extra);
      if (parsed != 3 ||
          !std::isfinite(target.x) || !std::isfinite(target.y) ||
          !std::isfinite(target.z) || std::fabs(target.x) > 10000000.0f ||
          std::fabs(target.y) > 10000000.0f ||
          std::fabs(target.z) > 10000000.0f) {
        REXLOG_WARN("[cheat] usage: cheat_teleport <finite-x> <finite-y> "
                    "<finite-z>");
        return;
      }
      thps::guest_call::Post([target]() mutable {
        uint32_t skater = 0;
        thps::cheats::Position before;
        if (!thps::cheats::GetLocalSkaterOnGuestThread(skater) ||
            !thps::cheats::ReadPosition(skater, before)) {
          REXLOG_WARN("[cheat] teleport: local skater position unavailable");
          return;
        }
        target.w = before.w;
        if (!thps::cheats::WritePosition(skater, target)) {
          REXLOG_WARN("[cheat] raw position target is not mapped");
          return;
        }
        thps::cheats::detail::state().teleports.fetch_add(
            1, std::memory_order_relaxed);
        REXLOG_INFO("[cheat] raw position write {:08X}: "
                    "{:.3f} {:.3f} {:.3f} -> {:.3f} {:.3f} {:.3f}",
                    skater, before.x, before.y, before.z,
                    target.x, target.y, target.z);
      });
    }),
    "Cheats", "cheat_teleport <x> <y> <z> - raw position diagnostic; game "
              "state may restore it");

REXCVAR_DEFINE_COMMAND_ARGS(
    cheat_spawn_script,
    ([](std::string_view args) {
      std::string name = thps::cheats::CleanArgument(args);
      if (name.empty() || name.find_first_of(" \t\r\n") != std::string::npos) {
        REXLOG_WARN("[cheat] usage: cheat_spawn_script <script-name>");
        return;
      }
      thps::guest_call::Post([name = std::move(name)]() {
        thps::cheats::SpawnScriptOnGuestThread(name);
      });
    }),
    "Cheats", "cheat_spawn_script <name> - start a shipped QB script without "
              "parameters");

REXCVAR_DEFINE_COMMAND_ARGS(
    cheat_zone,
    ([](std::string_view args) {
      std::string zone = thps::cheats::CleanArgument(args);
      if (zone != "funpark" && zone != "Funpark") {
        REXLOG_WARN("[cheat] usage: cheat_zone funpark");
        return;
      }
      thps::guest_call::Post([]() {
        constexpr uint32_t kLevel = 0x651533EC;
        constexpr uint32_t kLoadFunpark = 0x2349A9C0;
        constexpr uint32_t kNotFromLevelSelectMenu = 0x856E853E;
        // The shipped Free Skate READY handler calls level_select_change_level
        // with level=load_z_houses and not_from_levelselect_menu. Reproduce
        // that exact launch contract with load_z_funpark. Unlike Zone_Changed,
        // this path owns the full loading-screen, pak, world, node, ped, and
        // setup sequence, so it is valid both from the frontend and in-level.
        thps::cheats::GuestStruct params;
        if (!params.valid() ||
            !params.AddChecksum(kLevel, kLoadFunpark) ||
            !params.AddChecksum(kNotFromLevelSelectMenu,
                                kNotFromLevelSelectMenu)) {
          REXLOG_WARN("[cheat] could not build Funpark level-select parameters");
          return;
        }
        const uint32_t result = thps::cheats::SpawnScriptOnGuestThread(
            "level_select_change_level", params.address());
        if (result) {
          thps::cheats::detail::state().zone_changes.fetch_add(
              1, std::memory_order_relaxed);
        }
        REXLOG_INFO("[cheat] level_select_change_level(load_z_funpark) "
                    "returned {:08X}", result);
      });
    }),
    "Cheats", "cheat_zone funpark - load Funpark through the retail Free Skate "
              "level-selection path");

REXCVAR_DEFINE_COMMAND_ARGS(
    cheat_warp,
    ([](std::string_view args) {
      std::string node = thps::cheats::CleanArgument(args);
      if (node.empty()) {
        REXLOG_WARN("[cheat] usage: cheat_warp <restart-node-name>");
        return;
      }
      thps::guest_call::Post([node = std::move(node)]() {
        thps::cheats::GuestStruct params;
        const uint32_t node_checksum = thps::probe::Checksum(node);
        // Shipped dbg.pak.xen: 0x9F92BA78 -> "nodename". Retail
        // AddWarpPointsToMenu stores this exact CStruct as pad_choose_params
        // beside pad_choose_script=WarpSkater. The menu selection then queues
        // WarpSkater through the same asynchronous constructor used here.
        if (!params.valid() || !params.AddChecksum(0x9F92BA78, node_checksum)) {
          REXLOG_WARN("[cheat] could not build WarpSkater parameters");
          return;
        }
        const uint32_t warp_checksum = thps::probe::Checksum("WarpSkater");
        const uint32_t result = thps::cheats::SpawnScriptOnGuestThread(
            "WarpSkater", params.address());
        thps::cheats::detail::state().warps.fetch_add(
            1, std::memory_order_relaxed);
        REXLOG_INFO("[cheat] WarpSkater('{}', {:08X}; script {:08X}) returned "
                    "{:08X}", node, node_checksum, warp_checksum, result);
      });
    }),
    "Cheats", "cheat_warp <restart-node-name> - run the retail WarpSkater script");

REXCVAR_DEFINE_COMMAND_ARGS(
    cheat_warp_pos,
    ([](std::string_view args) {
      thps::cheats::Position target;
      char extra = 0;
      const int parsed = std::sscanf(std::string(args).c_str(), "%f %f %f %c",
                                     &target.x, &target.y, &target.z, &extra);
      if (parsed != 3 || !std::isfinite(target.x) ||
          !std::isfinite(target.y) || !std::isfinite(target.z) ||
          std::fabs(target.x) > 10000000.0f ||
          std::fabs(target.y) > 10000000.0f ||
          std::fabs(target.z) > 10000000.0f) {
        REXLOG_WARN("[cheat] usage: cheat_warp_pos <finite-x> <finite-y> "
                    "<finite-z>");
        return;
      }
      thps::guest_call::Post([target]() {
        thps::cheats::GuestStruct params;
        // The shipped WarpSkater script forwards all parameters to the
        // skater-bound TeleportSkaterToNode script. That script accepts either
        // nodename or pos; the latter calls obj_movetopos and updates the full
        // skater/physics state instead of only poking the render position.
        constexpr uint32_t kPos = 0x7F261953;
        if (!params.valid() ||
            !params.AddVector(kPos, target.x, target.y, target.z)) {
          REXLOG_WARN("[cheat] could not build WarpSkater position parameters");
          return;
        }
        const uint32_t result = thps::cheats::SpawnScriptOnGuestThread(
            "WarpSkater", params.address());
        thps::cheats::detail::state().warps.fetch_add(
            1, std::memory_order_relaxed);
        REXLOG_INFO("[cheat] WarpSkater(pos {:.3f} {:.3f} {:.3f}) returned "
                    "{:08X}", target.x, target.y, target.z, result);
      });
    }),
    "Cheats", "cheat_warp_pos <x> <y> <z> - move through the retail "
              "skater-bound teleport script");

REXCVAR_DEFINE_COMMAND(
    cheat_where,
    ([]() {
      // IsMoviePlaying is classified as parameter-free but hangs when invoked
      // outside the script VM's own call chain. Keep this command to the two
      // predicates proven safe in the Deck fixture.
      thps::guest_call::Post([]() {
        static constexpr const char* kProbes[] = {
            "InFrontend", "IsCareerMode"};
        for (const char* p : kProbes) {
          uint32_t v = 0;
          if (thps::cheats::CallPredicateOnGuestThread(p, v))
            REXLOG_INFO("[cheat] {:<18} = {}", p, v);
          else
            REXLOG_WARN("[cheat] {:<18} unavailable", p);
        }
      });
    }),
    "Cheats", "Report the game's proven-safe frontend and career predicates");

REXCVAR_DEFINE_COMMAND(
    cheat_memstats,
    ([]() {
      // The retail build still carries Neversoft's own heap reporting - the
      // "Debug: total unused memory" family of strings is right there in the
      // image. These dump to the log.
      thps::guest_call::Post([]() {
        for (const char* p : {"DumpMemStatistics", "DumpHavokMemStats",
                              "DumpCOIMEntries"}) {
          uint32_t v = 0;
          if (!thps::cheats::CallPredicateOnGuestThread(p, v))
            REXLOG_WARN("[cheat] {} unavailable", p);
        }
      });
      REXLOG_INFO("[cheat] memory dumps posted; output follows on the guest thread");
    }),
    "Cheats", "Dump the engine's own heap/Havok/COIM memory statistics to the log");

REXCVAR_DEFINE_BOOL(cheat_skip_intro, false, "Cheats",
                    "Skip the boot movies only. Bounded by the game's own "
                    "InFrontend predicate, so it stops the instant the main menu "
                    "is reached and can never touch a gameplay cutscene.");

namespace thps::cheats {

// Intro skip, scoped by construction.
//
// The obvious lever, `SetEnableMovies`, is the WRONG one: it is a global switch,
// so it would also kill the FMVs that play during career gameplay. The
// requirement is to skip the boot sequence and nothing else.
//
// So the scope is not a policy we promise to honour, it is the shape of the
// mechanism: the task lives only while `InFrontend()` reports false, and
// retires permanently the first time it reports true. After the main menu is
// reached there is no code left running to affect anything, so a mid-career
// cutscene is untouchable by it even in principle.
//
// The skip ACTION is still missing: `KillMovie` reads its parameter struct, but
// its required key and value type have not been recovered. GuestStruct removes
// the representation blocker; it does not justify inventing a schema. Until
// that schema is identified (or scripted START is scoped to this watch), this
// task observes and reports the boot movie sequence rather than cutting it
// short. That is deliberately visible in the log instead of silently doing
// nothing.
inline void StartIntroSkipWatch() {
  guest_call::Post([]() {
    uint32_t in_frontend = 0;
    if (CallPredicateOnGuestThread("InFrontend", in_frontend) && in_frontend) {
      REXLOG_INFO("[cheat] skip-intro: frontend reached, watch retired");
      return;
    }
    uint32_t playing = 0;
    if (CallPredicateOnGuestThread("IsMoviePlaying", playing) && playing) {
      static uint64_t last_report = 0;
      const uint64_t now = guest_call::frames();
      if (now - last_report > 120) {
        last_report = now;
        REXLOG_INFO("[cheat] skip-intro: boot movie playing at frame {}; no skip "
                    "action available yet (KillMovie schema is unknown)", now);
      }
    }
    StartIntroSkipWatch();  // still pre-frontend: look again next frame
  });
}

}  // namespace thps::cheats
