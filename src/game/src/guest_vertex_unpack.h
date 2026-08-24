// Optional exact replacement for THP8's hot vertex-format record unpack.
//
// sub_82354BE0 expands a 252-byte title record into a 496-byte render-side
// record. Most of the generated cost is PPC vector-register bookkeeping for
// fixed copies and three small packed formats. The generated function remains
// the default and an in-process oracle; the native path is eligible for timing
// only after the complete destination record compares byte-for-byte.

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define THPS_VERTEX_UNPACK_X86_SIMD 1
#if defined(__GNUC__) || defined(__clang__)
#define THPS_VERTEX_UNPACK_SIMD_TARGET __attribute__((target("ssse3,sse4.1")))
#else
#define THPS_VERTEX_UNPACK_SIMD_TARGET
#endif
#else
#define THPS_VERTEX_UNPACK_X86_SIMD 0
#define THPS_VERTEX_UNPACK_SIMD_TARGET
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(guest_vertex_unpack_native, false, "Guest",
                    "Replace sub_82354BE0's vertex-format record unpack with "
                    "an exact host implementation");
REXCVAR_DEFINE_BOOL(guest_vertex_unpack_verify, false, "Guest",
                    "Compare the native vertex-format unpack against the "
                    "generated guest function");
REXCVAR_DEFINE_BOOL(guest_vertex_unpack_stats, false, "Guest",
                    "Log vertex-format unpack calls and verification results");
REXCVAR_DEFINE_BOOL(guest_vertex_unpack_simd, false, "Guest",
                    "Use the exact x86 SIMD vertex-format unpack after the "
                    "native replacement is enabled");

namespace thps::vertex_unpack {

inline constexpr uint32_t kU8InitFlag = 0x827E6020;
inline constexpr uint32_t kAlignedU8InitFlag = 0x827E6040;
inline constexpr uint32_t kPackedInitFlag = 0x827E6090;
inline constexpr size_t kInputSize = 252;
inline constexpr size_t kOutputSize = 496;

inline std::atomic<uint64_t> g_calls{0};
inline std::atomic<uint64_t> g_native_calls{0};
inline std::atomic<uint64_t> g_simd_calls{0};
inline std::atomic<uint64_t> g_verified_calls{0};
inline std::atomic<uint64_t> g_mismatches{0};
inline std::atomic<uint64_t> g_init_calls{0};
inline std::atomic<uint64_t> g_fallbacks{0};
inline std::atomic<bool> g_detailed_mismatch_logged{false};

inline uint32_t ReadBe32(const uint8_t *source) {
  return (uint32_t{source[0]} << 24) | (uint32_t{source[1]} << 16) |
         (uint32_t{source[2]} << 8) | uint32_t{source[3]};
}

inline void WriteBe32(uint8_t *destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value >> 24);
  destination[1] = static_cast<uint8_t>(value >> 16);
  destination[2] = static_cast<uint8_t>(value >> 8);
  destination[3] = static_cast<uint8_t>(value);
}

inline void WriteFloat(uint8_t *destination, float value) {
  WriteBe32(destination, std::bit_cast<uint32_t>(value));
}

inline void DecodeU8x4(const uint8_t *source, uint8_t *destination) {
  constexpr float kScale = 1.0f / 256.0f;
  for (uint32_t i = 0; i < 4; ++i) {
    WriteFloat(destination + i * 4, static_cast<float>(source[i]) * kScale);
  }
}

inline void DecodePacked(const uint8_t *source, uint8_t *destination) {
  const uint32_t packed = ReadBe32(source);
  const int32_t x = static_cast<int32_t>(packed << 22) >> 22;
  const int32_t y = static_cast<int32_t>(packed << 11) >> 21;
  const int32_t z = static_cast<int32_t>(packed) >> 21;

  WriteFloat(destination + 0,
             static_cast<float>(x) * std::bit_cast<float>(0x3B004020u));
  WriteFloat(destination + 4,
             static_cast<float>(y) * std::bit_cast<float>(0x3A802008u));
  WriteFloat(destination + 8,
             static_cast<float>(z) * std::bit_cast<float>(0x3A802008u));
  // The guest converts the whole signed packed word to float and multiplies
  // lane 3 by +0.0f. Preserve the resulting negative zero for words whose
  // high bit is set.
  WriteBe32(destination + 12, packed & 0x80000000u);
}

inline void CopyXyzNegativeZeroW(const uint8_t *source, uint8_t *destination) {
  std::memcpy(destination, source, 12);
  WriteBe32(destination + 12, 0x80000000u);
}

inline void CopyXyzPositiveZeroW(const uint8_t *source, uint8_t *destination) {
  std::memcpy(destination, source, 12);
  WriteBe32(destination + 12, 0);
}

inline void SplatWord(const uint8_t *source, uint8_t *destination) {
  for (uint32_t i = 0; i < 4; ++i) {
    std::memcpy(destination + i * 4, source, 4);
  }
}

inline void TransformScalar(const uint8_t *source, uint8_t *destination) {
  DecodeU8x4(source + 244, destination + 0);
  DecodePacked(source + 220, destination + 16);
  DecodeU8x4(source + 224, destination + 32);
  DecodePacked(source + 232, destination + 48);
  DecodeU8x4(source + 236, destination + 64);

  CopyXyzNegativeZeroW(source + 0, destination + 80);
  DecodeU8x4(source + 0, destination + 96);
  CopyXyzNegativeZeroW(source + 16, destination + 112);
  CopyXyzNegativeZeroW(source + 32, destination + 128);
  DecodeU8x4(source + 32, destination + 144);
  CopyXyzNegativeZeroW(source + 48, destination + 160);
  DecodePacked(source + 76, destination + 176);
  std::memcpy(destination + 192, source + 64, 16);
  DecodeU8x4(source + 92, destination + 208);
  std::memcpy(destination + 224, source + 80, 16);
  std::memcpy(destination + 240, source + 96, 16);

  if (source[248] & 1) {
    for (uint32_t i = 0; i < 9; ++i) {
      // The retail unaligned-vector sequence retains the following word when
      // each four-row group begins on a 16-byte boundary. Preserve that exact
      // result rather than normalizing every row to homogeneous XYZ0.
      if ((i & 3) == 0) {
        std::memcpy(destination + 256 + i * 16, source + 112 + i * 12, 16);
      } else {
        CopyXyzPositiveZeroW(source + 112 + i * 12, destination + 256 + i * 16);
      }
    }
  }

  SplatWord(source + 228, destination + 400);
  SplatWord(source + 240, destination + 416);
  SplatWord(source + 16, destination + 432);
  SplatWord(source + 48, destination + 448);
  SplatWord(source + 108, destination + 464);

  const uint32_t flags = ReadBe32(source + 248);
  WriteBe32(destination + 480, std::rotl(flags, 2) & 0x3);
  WriteBe32(destination + 484, std::rotl(flags, 5) & 0x7);
  WriteBe32(destination + 488, std::rotl(flags, 7) & 0x3);
  WriteBe32(destination + 492, source[248] & 1);
}

#if THPS_VERTEX_UNPACK_X86_SIMD
THPS_VERTEX_UNPACK_SIMD_TARGET inline __m128i ByteSwapWords(__m128i value) {
  const __m128i shuffle =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  return _mm_shuffle_epi8(value, shuffle);
}

THPS_VERTEX_UNPACK_SIMD_TARGET inline void
DecodeU8x4Simd(const uint8_t *source, uint8_t *destination) {
  uint32_t bytes;
  std::memcpy(&bytes, source, sizeof(bytes));
  const __m128i integers = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(bytes));
  const __m128 values =
      _mm_mul_ps(_mm_cvtepi32_ps(integers), _mm_set1_ps(1.0f / 256.0f));
  _mm_storeu_si128(reinterpret_cast<__m128i *>(destination),
                   ByteSwapWords(_mm_castps_si128(values)));
}

THPS_VERTEX_UNPACK_SIMD_TARGET inline void
DecodePackedSimd(const uint8_t *source, uint8_t *destination) {
  const uint32_t packed = ReadBe32(source);
  const int32_t x = static_cast<int32_t>(packed << 22) >> 22;
  const int32_t y = static_cast<int32_t>(packed << 11) >> 21;
  const int32_t z = static_cast<int32_t>(packed) >> 21;
  const __m128 scales = _mm_set_ps(0.0f, std::bit_cast<float>(0x3A802008u),
                                   std::bit_cast<float>(0x3A802008u),
                                   std::bit_cast<float>(0x3B004020u));
  __m128i values = _mm_castps_si128(
      _mm_mul_ps(_mm_cvtepi32_ps(_mm_set_epi32(0, z, y, x)), scales));
  values = _mm_or_si128(
      values,
      _mm_set_epi32(static_cast<int32_t>(packed & 0x80000000u), 0, 0, 0));
  _mm_storeu_si128(reinterpret_cast<__m128i *>(destination),
                   ByteSwapWords(values));
}

THPS_VERTEX_UNPACK_SIMD_TARGET inline void SplatWordSimd(const uint8_t *source,
                                                         uint8_t *destination) {
  uint32_t word;
  std::memcpy(&word, source, sizeof(word));
  _mm_storeu_si128(reinterpret_cast<__m128i *>(destination),
                   _mm_set1_epi32(static_cast<int32_t>(word)));
}

THPS_VERTEX_UNPACK_SIMD_TARGET inline void TransformSimd(const uint8_t *source,
                                                         uint8_t *destination) {
  DecodeU8x4Simd(source + 244, destination + 0);
  DecodePackedSimd(source + 220, destination + 16);
  DecodeU8x4Simd(source + 224, destination + 32);
  DecodePackedSimd(source + 232, destination + 48);
  DecodeU8x4Simd(source + 236, destination + 64);

  CopyXyzNegativeZeroW(source + 0, destination + 80);
  DecodeU8x4Simd(source + 0, destination + 96);
  CopyXyzNegativeZeroW(source + 16, destination + 112);
  CopyXyzNegativeZeroW(source + 32, destination + 128);
  DecodeU8x4Simd(source + 32, destination + 144);
  CopyXyzNegativeZeroW(source + 48, destination + 160);
  DecodePackedSimd(source + 76, destination + 176);
  std::memcpy(destination + 192, source + 64, 16);
  DecodeU8x4Simd(source + 92, destination + 208);
  std::memcpy(destination + 224, source + 80, 16);
  std::memcpy(destination + 240, source + 96, 16);

  if (source[248] & 1) {
    for (uint32_t i = 0; i < 9; ++i) {
      if ((i & 3) == 0) {
        std::memcpy(destination + 256 + i * 16, source + 112 + i * 12, 16);
      } else {
        CopyXyzPositiveZeroW(source + 112 + i * 12, destination + 256 + i * 16);
      }
    }
  }

  SplatWordSimd(source + 228, destination + 400);
  SplatWordSimd(source + 240, destination + 416);
  SplatWordSimd(source + 16, destination + 432);
  SplatWordSimd(source + 48, destination + 448);
  SplatWordSimd(source + 108, destination + 464);

  const uint32_t flags = ReadBe32(source + 248);
  WriteBe32(destination + 480, std::rotl(flags, 2) & 0x3);
  WriteBe32(destination + 484, std::rotl(flags, 5) & 0x7);
  WriteBe32(destination + 488, std::rotl(flags, 7) & 0x3);
  WriteBe32(destination + 492, source[248] & 1);
}
#endif

inline constexpr bool kHasSimd = THPS_VERTEX_UNPACK_X86_SIMD != 0;

inline void Transform(const uint8_t *source, uint8_t *destination,
                      bool use_simd) {
#if THPS_VERTEX_UNPACK_X86_SIMD
  if (use_simd) {
    TransformSimd(source, destination);
    return;
  }
#else
  (void)use_simd;
#endif
  TransformScalar(source, destination);
}

inline bool Overlaps(uint32_t source, uint32_t destination) {
  const uint64_t source_end = uint64_t{source} + kInputSize;
  const uint64_t destination_end = uint64_t{destination} + kOutputSize;
  return uint64_t{source} < destination_end &&
         uint64_t{destination} < source_end;
}

inline bool TablesInitialized(uint8_t *base) {
  return (REX_LOAD_U32(kU8InitFlag) & 1) &&
         (REX_LOAD_U32(kAlignedU8InitFlag) & 1) &&
         (REX_LOAD_U32(kPackedInitFlag) & 3) == 3;
}

inline void MaybeLog(uint64_t calls) {
  if (!REXCVAR_GET(guest_vertex_unpack_stats) || (calls & 0xFFFFF) != 0) {
    return;
  }
  REXLOG_INFO("guest-vertex-unpack: calls={} native={} simd={} verified={} "
              "mismatches={} init_calls={} fallbacks={}",
              calls, g_native_calls.load(std::memory_order_relaxed),
              g_simd_calls.load(std::memory_order_relaxed),
              g_verified_calls.load(std::memory_order_relaxed),
              g_mismatches.load(std::memory_order_relaxed),
              g_init_calls.load(std::memory_order_relaxed),
              g_fallbacks.load(std::memory_order_relaxed));
}

} // namespace thps::vertex_unpack

// Generated guest functions expose their original bodies through weak ELF
// aliases. Mach-O has no equivalent C/C++ alias attribute, so retain the
// generated function on Apple while keeping the cvars parseable.
#if !defined(__APPLE__)
REX_HOOK_RAW(sub_82354BE0) {
  REX_FUNC_PROLOGUE();

  const bool use_native = REXCVAR_GET(guest_vertex_unpack_native);
  if (!use_native) {
    const bool verify = REXCVAR_GET(guest_vertex_unpack_verify);
    const bool stats = REXCVAR_GET(guest_vertex_unpack_stats);
    const uint64_t calls = stats ? thps::vertex_unpack::g_calls.fetch_add(
                                       1, std::memory_order_relaxed) +
                                       1
                                 : 0;
    const uint32_t destination = ctx.r3.u32;
    const uint32_t source = ctx.r4.u32;
    std::array<uint8_t, thps::vertex_unpack::kInputSize> input{};
    std::array<uint8_t, thps::vertex_unpack::kOutputSize> before{};
    if (verify) {
      std::memcpy(input.data(), REX_RAW_ADDR(source), input.size());
      std::memcpy(before.data(), REX_RAW_ADDR(destination), before.size());
    }

    __imp__sub_82354BE0(ctx, base);

    if (verify) {
      auto candidate = before;
      const bool use_simd = REXCVAR_GET(guest_vertex_unpack_simd) &&
                            thps::vertex_unpack::kHasSimd;
      thps::vertex_unpack::Transform(input.data(), candidate.data(), use_simd);
      if (use_simd && stats) {
        thps::vertex_unpack::g_simd_calls.fetch_add(1,
                                                    std::memory_order_relaxed);
      }
      const auto *observed =
          reinterpret_cast<const uint8_t *>(REX_RAW_ADDR(destination));
      uint64_t mismatches = 0;
      uint32_t first = 0;
      for (uint32_t i = 0; i < candidate.size(); ++i) {
        if (candidate[i] != observed[i]) {
          if (!mismatches) {
            first = i;
          }
          ++mismatches;
        }
      }
      thps::vertex_unpack::g_verified_calls.fetch_add(
          1, std::memory_order_relaxed);
      if (mismatches) {
        const uint64_t total = thps::vertex_unpack::g_mismatches.fetch_add(
                                   mismatches, std::memory_order_relaxed) +
                               mismatches;
        if (!thps::vertex_unpack::g_detailed_mismatch_logged.exchange(
                true, std::memory_order_relaxed)) {
          for (uint32_t i = 0; i < candidate.size(); ++i) {
            if (candidate[i] != observed[i]) {
              REXLOG_ERROR("guest-vertex-unpack detail: offset={} "
                           "expected={:02X} observed={:02X}",
                           i, candidate[i], observed[i]);
            }
          }
        }
        if (total <= 64) {
          REXLOG_ERROR("guest-vertex-unpack mismatch: src={:08X} dst={:08X} "
                       "first={} expected={:02X} observed={:02X} count={}",
                       source, destination, first, candidate[first],
                       observed[first], mismatches);
        }
      }
    }
    thps::vertex_unpack::MaybeLog(calls);
    return;
  }

  const uint32_t destination = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;
  if (!thps::vertex_unpack::TablesInitialized(base)) {
    thps::vertex_unpack::g_init_calls.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82354BE0(ctx, base);
    return;
  }
  if ((source & 0xF) || (destination & 0xF) ||
      thps::vertex_unpack::Overlaps(source, destination)) {
    thps::vertex_unpack::g_fallbacks.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82354BE0(ctx, base);
    return;
  }

  const bool use_simd =
      REXCVAR_GET(guest_vertex_unpack_simd) && thps::vertex_unpack::kHasSimd;
  thps::vertex_unpack::Transform(
      reinterpret_cast<const uint8_t *>(REX_RAW_ADDR(source)),
      reinterpret_cast<uint8_t *>(REX_RAW_ADDR(destination)), use_simd);
  ctx.r3.u64 = std::rotl(REX_LOAD_U32(source + 248), 7) & 0x3;
  ctx.fpscr.enableFlushMode();

  if (REXCVAR_GET(guest_vertex_unpack_stats)) {
    const uint64_t calls =
        thps::vertex_unpack::g_calls.fetch_add(1, std::memory_order_relaxed) +
        1;
    thps::vertex_unpack::g_native_calls.fetch_add(1, std::memory_order_relaxed);
    if (use_simd) {
      thps::vertex_unpack::g_simd_calls.fetch_add(1, std::memory_order_relaxed);
    }
    thps::vertex_unpack::MaybeLog(calls);
  }
}
#endif

#undef THPS_VERTEX_UNPACK_X86_SIMD
#undef THPS_VERTEX_UNPACK_SIMD_TARGET
