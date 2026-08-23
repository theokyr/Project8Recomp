// Guest-memory probe: read, write, search, and the script-command table.
//
// This is the reverse-engineering instrument the cheats are built on, exposed
// as console commands so the game itself becomes the tool. Static reading of
// the generated C++ finds functions well and finds live object addresses badly;
// a search over a running process finds them in seconds.
//
// THE SCRIPT-COMMAND TABLE
//
// The engine (nxcommon) publishes its C++ functions to QB scripts through an
// array of `{const char* name; handler;}` 8-byte records. Retail Project 8
// ships it with the names intact: 1,521 entries at guest VA 0x826D9C68, which
// is 1,366 distinct guest functions with real names - `ChangeLevel`,
// `RestartLevel`, `GetSkaterPosition`, `TryCheatString`. The names and handler
// ranges were independently validated against the generated function map.
//
// Resolving it at RUNTIME rather than hardcoding the addresses is deliberate:
//
//   * one address is checked instead of hundreds, and
//   * the check is self-validating. The names have to read back as ASCII and
//     the handlers have to land in .text, so the wrong dump fails loudly at
//     `script_table` instead of quietly warping into nothing.
//
// 144 of the names point at two shared no-op stubs (0x822B93D0 and
// 0x822B9708): debug commands compiled out of the retail build, kept in the
// table so scripts referencing them still resolve. `script_find` marks them,
// because calling one and seeing nothing happen is otherwise indistinguishable
// from a cheat that failed.
//
// SAFETY OF HOST WRITES
//
// A host write through TranslateVirtual bypasses the GPU write-watch fault
// path that guest stores go through, so writing
// into GPU-visible physical memory would leave the GPU reading stale data.
// `poke` therefore refuses addresses outside the guest virtual heap, and every
// cheat write is performed from the guest frame pump rather than the UI thread
// so it cannot race the guest's own update of the same field.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "guest_call.h"

namespace thps::probe {

// Overridable so a different dump can be pointed at its own table without a
// rebuild.
inline constexpr uint32_t kScriptTableDefault = 0x826D9C68;
// Observed image ranges: .rdata 0x82000400 + 0x5FEDC; code spans .text
// 0x82090000 and BINK up to 0x826CD284.
inline constexpr uint32_t kRdataLo = 0x82000400;
inline constexpr uint32_t kRdataHi = 0x82000400 + 0x5FEDC;
inline constexpr uint32_t kCodeLo = 0x82090000;
inline constexpr uint32_t kCodeHi = 0x826CD284;
// The two retail no-op stubs; see the header comment.
inline constexpr uint32_t kStubA = 0x822B93D0;
inline constexpr uint32_t kStubB = 0x822B9708;

namespace detail {

struct State {
  rex::Runtime* runtime = nullptr;
  std::vector<std::pair<std::string, uint32_t>> commands;  // name -> handler
  bool table_scanned = false;
};

inline State& state() {
  static State s;
  return s;
}

}  // namespace detail

inline void SetRuntime(rex::Runtime* runtime) { detail::state().runtime = runtime; }

// Never cached: the heap may not be mapped when a command is first typed, and a
// stale null would then be wrong forever. Same discipline as
// parity_capture.h::ResolveGuest.
inline uint8_t* Resolve(uint32_t va) {
  auto& s = detail::state();
  if (!va || !s.runtime || !s.runtime->memory()) return nullptr;
  auto* memory = s.runtime->memory();
  if (!memory->LookupHeap(va)) return nullptr;
  return memory->TranslateVirtual<uint8_t*>(va);
}

inline bool ReadU32(uint32_t va, uint32_t& out) {
  const uint8_t* p = Resolve(va);
  if (!p) return false;
  uint32_t raw = 0;
  std::memcpy(&raw, p, sizeof(raw));
  out = rex::byte_swap(raw);
  return true;
}

inline bool ReadU8(uint32_t va, uint8_t& out) {
  const uint8_t* p = Resolve(va);
  if (!p) return false;
  out = *p;
  return true;
}

inline bool WriteU32(uint32_t va, uint32_t value) {
  uint8_t* p = Resolve(va);
  if (!p) return false;
  const uint32_t raw = rex::byte_swap(value);
  std::memcpy(p, &raw, sizeof(raw));
  return true;
}

inline bool ReadFloat(uint32_t va, float& out) {
  uint32_t raw = 0;
  if (!ReadU32(va, raw)) return false;
  std::memcpy(&out, &raw, sizeof(out));
  return true;
}

inline bool WriteFloat(uint32_t va, float value) {
  uint32_t raw = 0;
  std::memcpy(&raw, &value, sizeof(raw));
  return WriteU32(va, raw);
}

// Reads a NUL-terminated ASCII string out of guest memory, bounded.
inline bool ReadCString(uint32_t va, std::string& out, size_t max = 96) {
  const uint8_t* p = Resolve(va);
  if (!p) return false;
  out.clear();
  for (size_t i = 0; i < max; ++i) {
    const char c = static_cast<char>(p[i]);
    if (c == '\0') return !out.empty();
    if (c < 32 || c >= 127) return false;
    out.push_back(c);
  }
  return false;
}

inline bool IsStub(uint32_t handler) { return handler == kStubA || handler == kStubB; }

// Walks the script-command table out of live guest memory. Returns the number
// of entries, 0 when the table does not validate.
inline size_t ScanScriptTable(uint32_t table_va = kScriptTableDefault) {
  auto& s = detail::state();
  s.commands.clear();
  s.table_scanned = true;
  for (uint32_t off = 0;; off += 8) {
    uint32_t name_va = 0, handler = 0;
    if (!ReadU32(table_va + off, name_va)) break;
    if (!ReadU32(table_va + off + 4, handler)) break;
    if (name_va < kRdataLo || name_va >= kRdataHi) break;
    if (handler < kCodeLo || handler >= kCodeHi) break;
    std::string name;
    if (!ReadCString(name_va, name)) break;
    s.commands.emplace_back(std::move(name), handler);
    if (s.commands.size() > 4096) break;  // runaway guard
  }
  return s.commands.size();
}

inline const std::vector<std::pair<std::string, uint32_t>>& commands() {
  return detail::state().commands;
}

inline uint32_t Lookup(std::string_view name) {
  for (const auto& [n, addr] : detail::state().commands)
    if (n == name) return addr;
  return 0;
}

// Case-insensitive, because nobody types `GetSkaterPosition` correctly twice.
inline uint32_t LookupCaseless(std::string_view name) {
  auto lower = [](std::string_view v) {
    std::string o(v);
    for (char& c : o) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return o;
  };
  const std::string want = lower(name);
  for (const auto& [n, addr] : detail::state().commands)
    if (lower(n) == want) return addr;
  return 0;
}

// CRC-32/JAMCRC as the engine computes it: standard CRC-32 with the FINAL
// COMPLEMENT OMITTED, over the lowercased name with '/' normalised to '\'.
// engines/neversoft-nxcommon/docs/checksums.md verified this against 118,762
// of 118,762 name/checksum pairs harvested from the disc's .dbg assets, and
// disproved.md D4 records the trap: forget the missing complement and every
// lookup fails silently, because the values differ by exactly 0xFFFFFFFF.
inline uint32_t Checksum(std::string_view name) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    ready = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (char ch : name) {
    char c = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
    if (c == '/') c = '\\';
    crc = table[(crc ^ static_cast<uint8_t>(c)) & 0xFF] ^ (crc >> 8);
  }
  return crc;  // no final XOR - that is the whole point
}

}  // namespace thps::probe

// ---------------------------------------------------------------- commands

REXCVAR_DEFINE_COMMAND_ARGS(
    peek,
    ([](std::string_view args) {
      uint32_t va = 0, count = 1;
      char extra = 0;
      if (std::sscanf(std::string(args).c_str(), "%x %u %c", &va, &count, &extra) < 1) {
        REXLOG_WARN("[probe] usage: peek <hex-va> [words]");
        return;
      }
      if (count == 0 || count > 64) count = 1;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t word = 0;
        const uint32_t at = va + i * 4;
        if (!thps::probe::ReadU32(at, word)) {
          REXLOG_WARN("[probe] {:08X} unmapped", at);
          return;
        }
        float f = 0.0f;
        std::memcpy(&f, &word, sizeof(f));
        REXLOG_INFO("[probe] {:08X} = {:08X}  {}  {:g}", at, word,
                    static_cast<int32_t>(word), f);
      }
    }),
    "Dev", "peek <hex-va> [words] - read guest memory as hex/int/float");

REXCVAR_DEFINE_COMMAND_ARGS(
    poke,
    ([](std::string_view args) {
      uint32_t va = 0, value = 0;
      if (std::sscanf(std::string(args).c_str(), "%x %x", &va, &value) != 2) {
        REXLOG_WARN("[probe] usage: poke <hex-va> <hex-u32>");
        return;
      }
      // Performed on the guest frame pump, not here: a host write racing the
      // guest's own store to the same field is a real hazard, and the pump is a
      // known point in the frame.
      thps::guest_call::Post([va, value]() {
        if (thps::probe::WriteU32(va, value))
          REXLOG_INFO("[probe] {:08X} <- {:08X}", va, value);
        else
          REXLOG_WARN("[probe] {:08X} unmapped; not written", va);
      });
    }),
    "Dev", "poke <hex-va> <hex-u32> - write guest memory on the next guest frame");

REXCVAR_DEFINE_COMMAND_ARGS(
    script_table,
    ([](std::string_view args) {
      uint32_t va = thps::probe::kScriptTableDefault;
      if (!args.empty()) std::sscanf(std::string(args).c_str(), "%x", &va);
      const size_t n = thps::probe::ScanScriptTable(va);
      if (n == 0) {
        REXLOG_WARN("[probe] no script-command table at {:08X} - wrong dump, or "
                    "guest memory is not mapped yet", va);
        return;
      }
      size_t stubs = 0;
      for (const auto& [name, addr] : thps::probe::commands())
        if (thps::probe::IsStub(addr)) ++stubs;
      REXLOG_INFO("[probe] script table {:08X}: {} entries, {} are retail no-op "
                  "stubs", va, n, stubs);
    }),
    "Dev", "script_table [hex-va] - scan the engine's script-command table");

REXCVAR_DEFINE_COMMAND_ARGS(
    script_find,
    ([](std::string_view args) {
      if (thps::probe::commands().empty()) thps::probe::ScanScriptTable();
      const std::string needle(args);
      if (needle.empty()) {
        REXLOG_INFO("[probe] {} script commands known; give a substring to filter",
                    thps::probe::commands().size());
        return;
      }
      auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return v;
      };
      const std::string want = lower(needle);
      size_t shown = 0;
      for (const auto& [name, addr] : thps::probe::commands()) {
        if (lower(name).find(want) == std::string::npos) continue;
        REXLOG_INFO("[probe] {:08X} {} {}  (checksum {:08X})", addr,
                    thps::probe::IsStub(addr) ? "STUB" : "    ", name,
                    thps::probe::Checksum(name));
        if (++shown >= 40) {
          REXLOG_INFO("[probe] ... stopping at 40 matches");
          break;
        }
      }
      if (shown == 0) REXLOG_INFO("[probe] no script command matches '{}'", needle);
    }),
    "Dev", "script_find <substring> - search the script-command table");

REXCVAR_DEFINE_COMMAND_ARGS(
    checksum,
    ([](std::string_view args) {
      REXLOG_INFO("[probe] checksum(\"{}\") = {:08X}", args,
                  thps::probe::Checksum(args));
    }),
    "Dev", "checksum <name> - the engine's JAMCRC name checksum");
