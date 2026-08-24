// Optional exact replacement for one hot guest render-preparation helper.
//
// sub_82354398 expands four unsigned bytes into four guest floats, scaled by
// 1/256. The generated body performs a guest-side one-time shuffle-mask setup
// and then expresses the unpack through the full PPC vector-register model.
// This hook keeps the generated function as the default and as an in-process
// oracle. A separate verification mode compares a direct implementation with
// every generated result before the native path is eligible for timing.

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(guest_u8x4_unpack_native, false, "Guest",
                    "Replace sub_82354398's byte-to-float unpack with an "
                    "exact host implementation");
REXCVAR_DEFINE_BOOL(guest_u8x4_unpack_verify, false, "Guest",
                    "Compare the native byte-to-float unpack against the "
                    "generated guest function");
REXCVAR_DEFINE_BOOL(guest_u8x4_unpack_stats, false, "Guest",
                    "Log byte-to-float unpack calls and verification results");

namespace thps::u8x4_unpack {

inline constexpr uint32_t kInitFlag = 0x827E6020;
inline constexpr float kScale = 1.0f / 256.0f;

inline std::atomic<uint64_t> g_calls{0};
inline std::atomic<uint64_t> g_native_calls{0};
inline std::atomic<uint64_t> g_verified_calls{0};
inline std::atomic<uint64_t> g_mismatches{0};

inline std::array<uint32_t, 4> Decode(uint8_t *base, uint32_t source) {
  std::array<uint32_t, 4> result{};
  for (uint32_t i = 0; i < result.size(); ++i) {
    const float value = static_cast<float>(REX_LOAD_U8(source + i)) * kScale;
    result[i] = std::bit_cast<uint32_t>(value);
  }
  return result;
}

inline void Store(uint8_t *base, uint32_t destination,
                  const std::array<uint32_t, 4> &values) {
  for (uint32_t i = 0; i < values.size(); ++i) {
    REX_STORE_U32(destination + i * sizeof(uint32_t), values[i]);
  }
}

inline void MaybeLog(uint64_t calls) {
  if (!REXCVAR_GET(guest_u8x4_unpack_stats) || (calls & 0x3FFFFF) != 0) {
    return;
  }
  REXLOG_INFO("guest-u8x4-unpack: calls={} native={} verified={} mismatches={}",
              calls, g_native_calls.load(std::memory_order_relaxed),
              g_verified_calls.load(std::memory_order_relaxed),
              g_mismatches.load(std::memory_order_relaxed));
}

} // namespace thps::u8x4_unpack

// Generated guest functions expose their original bodies through weak ELF
// aliases. Mach-O has no equivalent C/C++ alias attribute, so retain the
// generated function on Apple while keeping the cvars parseable.
#if !defined(__APPLE__)
REX_HOOK_RAW(sub_82354398) {
  REX_FUNC_PROLOGUE();

  const bool use_native = REXCVAR_GET(guest_u8x4_unpack_native);
  if (!use_native) {
    const bool verify = REXCVAR_GET(guest_u8x4_unpack_verify);
    const bool stats = REXCVAR_GET(guest_u8x4_unpack_stats);
    const uint64_t calls = stats ? thps::u8x4_unpack::g_calls.fetch_add(
                                       1, std::memory_order_relaxed) +
                                       1
                                 : 0;
    const uint32_t source = ctx.r4.u32;
    const uint32_t destination = ctx.r3.u32 & ~uint32_t{0xF};
    const auto expected = verify ? thps::u8x4_unpack::Decode(base, source)
                                 : std::array<uint32_t, 4>{};

    __imp__sub_82354398(ctx, base);

    if (verify) {
      uint64_t mismatches = 0;
      std::array<uint32_t, 4> observed{};
      for (uint32_t i = 0; i < observed.size(); ++i) {
        observed[i] = REX_LOAD_U32(destination + i * sizeof(uint32_t));
        mismatches += observed[i] != expected[i];
      }
      thps::u8x4_unpack::g_verified_calls.fetch_add(1,
                                                    std::memory_order_relaxed);
      if (mismatches) {
        const uint64_t total = thps::u8x4_unpack::g_mismatches.fetch_add(
                                   mismatches, std::memory_order_relaxed) +
                               mismatches;
        if (total <= 32) {
          REXLOG_ERROR("guest-u8x4-unpack mismatch: src={:08X} dst={:08X} "
                       "expected={:08X},{:08X},{:08X},{:08X} "
                       "observed={:08X},{:08X},{:08X},{:08X}",
                       source, destination, expected[0], expected[1],
                       expected[2], expected[3], observed[0], observed[1],
                       observed[2], observed[3]);
        }
      }
    }
    thps::u8x4_unpack::MaybeLog(calls);
    return;
  }

  // Preserve the guest's one-time constant-table initialization. This is one
  // generated call per process, outside the steady-state optimization.
  if (!(REX_LOAD_U32(thps::u8x4_unpack::kInitFlag) & 1)) {
    __imp__sub_82354398(ctx, base);
    return;
  }

  thps::u8x4_unpack::Store(base, ctx.r3.u32 & ~uint32_t{0xF},
                           thps::u8x4_unpack::Decode(base, ctx.r4.u32));
  ctx.fpscr.enableFlushMode();
  const bool stats = REXCVAR_GET(guest_u8x4_unpack_stats);
  if (stats) {
    const uint64_t calls =
        thps::u8x4_unpack::g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    thps::u8x4_unpack::g_native_calls.fetch_add(1, std::memory_order_relaxed);
    thps::u8x4_unpack::MaybeLog(calls);
  }
}
#endif
