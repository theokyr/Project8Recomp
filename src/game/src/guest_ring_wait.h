// Replace one identified guest busy-wait with a bounded host backoff.
//
// sub_8235DF50 waits for work in a guest ring whose read and write indices are
// at +24 and +20. The generated function ends with three instructions that
// continuously reload and compare those words until a producer advances the
// write index. On the Steam Deck this function is the Main XThread's largest
// symbol and burns CPU without advancing the guest.
//
// sub_8235E068 is the producer: on a separate guest XThread it increments the
// write index under the ring's critical section. Strong definitions replace
// both weak codegen aliases. The consumer may either use the accepted bounded
// backoff or wait on the exact producer event. Defaults preserve generated
// behavior.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    guest_ring_wait_backoff, false, "Guest",
    "Back off while sub_8235DF50 waits for its guest ring producer");
REXCVAR_DEFINE_UINT32(guest_ring_wait_backoff_after, 1024, "Guest",
                      "Empty-ring polls before each host backoff");
REXCVAR_DEFINE_UINT32(guest_ring_wait_backoff_us, 1, "Guest",
                      "Host backoff duration in microseconds; zero yields");
REXCVAR_DEFINE_BOOL(guest_ring_wait_backoff_stats, false, "Guest",
                    "Log guest empty-ring backoff engagement");
REXCVAR_DEFINE_BOOL(guest_ring_wait_event, false, "Guest",
                    "Block the main thread until its guest ring producer "
                    "advances the write index");
REXCVAR_DEFINE_UINT32(guest_ring_wait_event_timeout_us, 5000, "Guest",
                      "Maximum host wait before rechecking the guest ring");
REXCVAR_DEFINE_BOOL(guest_ring_wait_event_stats, false, "Guest",
                    "Log guest ring event-wait engagement and duration");

namespace thps::ring_wait {

inline std::atomic<uint64_t> g_backoffs{0};
inline std::mutex g_event_mutex;
inline std::condition_variable g_produced;
inline std::atomic<uint64_t> g_event_waits{0};
inline std::atomic<uint64_t> g_event_timeouts{0};
inline std::atomic<uint64_t> g_event_total_us{0};
inline std::atomic<uint64_t> g_event_max_us{0};
inline std::atomic<uint64_t> g_producer_notifies{0};

inline bool HasWork(uint8_t *base, uint32_t ring) {
  return REX_LOAD_U32(ring + 24) != REX_LOAD_U32(ring + 20);
}

inline void RecordEventWait(uint64_t elapsed_us, bool produced) {
  if (!REXCVAR_GET(guest_ring_wait_event_stats)) {
    return;
  }

  const uint64_t waits =
      g_event_waits.fetch_add(1, std::memory_order_relaxed) + 1;
  g_event_total_us.fetch_add(elapsed_us, std::memory_order_relaxed);
  if (!produced) {
    g_event_timeouts.fetch_add(1, std::memory_order_relaxed);
  }

  uint64_t previous = g_event_max_us.load(std::memory_order_relaxed);
  while (previous < elapsed_us &&
         !g_event_max_us.compare_exchange_weak(previous, elapsed_us,
                                               std::memory_order_relaxed)) {
  }

  if ((waits & 0xFF) == 0) {
    REXLOG_INFO("guest-ring-event: waits={} mean_us={} max_us={} timeouts={} "
                "producer_notifies={}",
                waits, g_event_total_us.load(std::memory_order_relaxed) / waits,
                g_event_max_us.load(std::memory_order_relaxed),
                g_event_timeouts.load(std::memory_order_relaxed),
                g_producer_notifies.load(std::memory_order_relaxed));
  }
}

inline void BackOff() {
  const uint32_t micros = REXCVAR_GET(guest_ring_wait_backoff_us);
  if (micros) {
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
  } else {
    std::this_thread::yield();
  }

  if (REXCVAR_GET(guest_ring_wait_backoff_stats)) {
    const uint64_t count =
        g_backoffs.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((count & 0x3FFF) == 0) {
      REXLOG_INFO("guest-ring-wait: backoffs={}", count);
    }
  }
}

} // namespace thps::ring_wait

// Generated guest functions expose weak ELF aliases for title hooks. Mach-O
// has no equivalent C/C++ alias attribute, so Apple retains the generated
// functions while these cvars remain available to the shared launcher preset.
#if !defined(__APPLE__)
REX_HOOK_RAW(sub_8235DF50) {
  REX_FUNC_PROLOGUE();
  uint32_t ea{};
  uint32_t empty_polls = 0;
  ctx.r12.u64 = ctx.lr;
  ctx.lr = 0x8235DF58;
  __savegprlr_28(ctx, base);
  ea = -128 + ctx.r1.u32;
  REX_STORE_U32(ea, ctx.r1.u32);
  ctx.r1.u32 = ea;
  ctx.r28.u64 = ctx.r3.u64;
  ctx.r3.s64 = -388104192;
  ctx.r4.s64 = 0;
  ctx.r3.u64 = ctx.r3.u64 | 37972;
  ctx.lr = 0x8235DF70;
  sub_82216E58(ctx, base);
  ctx.r29.s64 = -2105540608;
  ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + -20112);
  ctx.cr6.compare<int32_t>(ctx.r3.s32, ctx.r11.s32, ctx.xer);
  if (ctx.cr6.eq)
    goto loc_8235DF8C;
  ctx.cr6.compare<uint32_t>(ctx.r3.u32, 2, ctx.xer);
  if (ctx.cr6.gt)
    goto loc_8235DF8C;
  REX_STORE_U32(ctx.r29.u32 + -20112, ctx.r3.u32);
loc_8235DF8C:
  ctx.lr = 0x8235DF90;
  sub_822FABF0(ctx, base);
  ctx.r11.s64 = -2106720256;
  ctx.r4.s64 = ctx.r11.s64 + 24176;
  ctx.lr = 0x8235DF9C;
  sub_82327E30(ctx, base);
  ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + -20112);
  ctx.cr6.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
  if (ctx.cr6.eq)
    goto loc_8235DFFC;
  ctx.lr = 0x8235DFAC;
  sub_82397698(ctx, base);
  ctx.r31.s64 = -2105540608;
  ctx.r30.s64 = -2105540608;
  ctx.r11.u64 = REX_LOAD_U64(ctx.r31.u32 + -20120);
  REX_STORE_U64(ctx.r30.u32 + -20128, ctx.r3.u64);
  ctx.r10.u64 = ctx.r11.u64 - ctx.r3.u64;
  ctx.cr6.compare<uint64_t>(ctx.r10.u64, 2, ctx.xer);
  if (!ctx.cr6.gt)
    goto loc_8235DFD0;
  ctx.r11.s64 = ctx.r3.s64 + 2;
  REX_STORE_U64(ctx.r31.u32 + -20120, ctx.r11.u64);
loc_8235DFD0:
  ctx.cr6.compare<uint64_t>(ctx.r3.u64, ctx.r11.u64, ctx.xer);
  if (!ctx.cr6.lt)
    goto loc_8235DFEC;
loc_8235DFD8:
  ctx.lr = 0x8235DFDC;
  sub_82397698(ctx, base);
  ctx.r11.u64 = REX_LOAD_U64(ctx.r31.u32 + -20120);
  REX_STORE_U64(ctx.r30.u32 + -20128, ctx.r3.u64);
  ctx.cr6.compare<uint64_t>(ctx.r3.u64, ctx.r11.u64, ctx.xer);
  if (ctx.cr6.lt)
    goto loc_8235DFD8;
loc_8235DFEC:
  ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + -20112);
  ctx.r11.s64 = ctx.r11.s32;
  ctx.r11.u64 = ctx.r11.u64 + ctx.r3.u64;
  REX_STORE_U64(ctx.r31.u32 + -20120, ctx.r11.u64);
loc_8235DFFC:
  ctx.r11.u64 = REX_LOAD_U32(ctx.r28.u32 + 24);
  ctx.r10.u64 = REX_LOAD_U32(ctx.r28.u32 + 20);
  ctx.cr6.compare<uint32_t>(ctx.r11.u32, ctx.r10.u32, ctx.xer);
  if (ctx.cr6.eq) {
    if (REXCVAR_GET(guest_ring_wait_event)) {
      std::unique_lock lock(thps::ring_wait::g_event_mutex);
      if (!thps::ring_wait::HasWork(base, ctx.r28.u32)) {
        const auto start = std::chrono::steady_clock::now();
        const bool produced = thps::ring_wait::g_produced.wait_for(
            lock,
            std::chrono::microseconds(
                REXCVAR_GET(guest_ring_wait_event_timeout_us)),
            [base, ring = ctx.r28.u32] {
              return thps::ring_wait::HasWork(base, ring);
            });
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start)
                .count();
        thps::ring_wait::RecordEventWait(
            static_cast<uint64_t>(elapsed < 0 ? 0 : elapsed), produced);
      }
      empty_polls = 0;
    } else {
      const uint32_t after = REXCVAR_GET(guest_ring_wait_backoff_after);
      if (REXCVAR_GET(guest_ring_wait_backoff) && after &&
          ++empty_polls >= after) {
        empty_polls = 0;
        thps::ring_wait::BackOff();
      }
    }
    goto loc_8235DFFC;
  }
  ctx.r1.s64 = ctx.r1.s64 + 128;
  __restgprlr_28(ctx, base);
}

REX_HOOK_RAW(sub_8235E068) {
  REX_FUNC_PROLOGUE();
  uint32_t ea{};

  const bool enabled = REXCVAR_GET(guest_ring_wait_event);
  std::unique_lock<std::mutex> event_lock(thps::ring_wait::g_event_mutex,
                                          std::defer_lock);
  if (enabled) {
    event_lock.lock();
  }

  // Generated sub_8235E068 body, kept verbatim.
  ctx.r12.u64 = ctx.lr;
  REX_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);
  REX_STORE_U64(ctx.r1.u32 + -24, ctx.r30.u64);
  REX_STORE_U64(ctx.r1.u32 + -16, ctx.r31.u64);
  ea = -112 + ctx.r1.u32;
  REX_STORE_U32(ea, ctx.r1.u32);
  ctx.r1.u32 = ea;
  ctx.r31.u64 = ctx.r3.u64;
  ctx.r30.s64 = ctx.r31.s64 + 56;
  ctx.r3.u64 = ctx.r30.u64;
  ctx.lr = 0x8235E08C;
  __imp__RtlEnterCriticalSection(ctx, base);
  ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 20);
  ctx.r3.u64 = ctx.r30.u64;
  ctx.r11.s64 = ctx.r11.s64 + 1;
  REX_STORE_U32(ctx.r31.u32 + 20, ctx.r11.u32);
  ctx.lr = 0x8235E0A0;
  __imp__RtlLeaveCriticalSection(ctx, base);
  ctx.r1.s64 = ctx.r1.s64 + 112;
  ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 + -8);
  ctx.lr = ctx.r12.u64;
  ctx.r30.u64 = REX_LOAD_U64(ctx.r1.u32 + -24);
  ctx.r31.u64 = REX_LOAD_U64(ctx.r1.u32 + -16);

  if (enabled) {
    if (REXCVAR_GET(guest_ring_wait_event_stats)) {
      thps::ring_wait::g_producer_notifies.fetch_add(1,
                                                     std::memory_order_relaxed);
    }
    event_lock.unlock();
    thps::ring_wait::g_produced.notify_one();
  }
}
#endif
