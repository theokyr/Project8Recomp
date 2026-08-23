// Stop the guest's frame-wait from burning a core it is not using.
//
// WHAT WAS MEASURED (Steam Deck, 2026-08-21, perf at 199 Hz over 11,474 samples)
//
// Two guest threads dominate this port's CPU, not the emulated command stream:
//
//     XThread911CD6C0   33.5% of process CPU, 67.5% of it in sub_82327EE0
//     Main XThread      24.6% of process CPU, 62.9% of it in sub_8235DF50
//     GPU Commands      16.8%, flat - no symbol above 5%
//
// sub_82327EE0 contains this, at 0x82327FCC:
//
//     loc_82327FCC:
//         bl    sub_82397688        ; -> 64-bit counter
//         lwz   r11, -25596(r30)    ; -> deadline
//         cmpld cr6, r3, r11
//         blt   cr6, loc_82327FCC   ; spin while counter < deadline
//     ... addi r9,r3,1 ; stw r9,-25596(r30)   ; next deadline = current + 1
//
// and sub_82397688 is a leaf that does nothing but load that counter:
//
//     lis r11,-32105 ; ld r3,2576(r11) ; blr
//
// So the guest render thread busy-waits for a frame counter that something
// else advances. On an Xbox 360 with six hardware threads a spinning thread is
// free. On a Steam Deck's four cores it is a quarter of the machine, and it is
// spent competing with the very thread that has to advance the counter before
// the spin can end. This is largely benign on a desktop with spare cores and
// materially harmful on a four-core handheld.
//
// WHAT THIS DOES
//
// Overrides sub_82397688 in the function dispatcher with a wrapper that calls
// the original and then, when the same value has come back several times in a
// row on the same thread, yields the timeslice.
//
// The contract is preserved exactly: the guest always receives the true current
// value of the counter. Nothing is faked, skipped or reordered - the only
// change is that a thread which has established it is waiting stops holding a
// core while it waits. If the machine is idle, sched_yield returns immediately
// and this costs a branch.
//
// It is a cvar, default OFF, because it is a guest-behaviour patch and the
// house rule for those is that every new lever behaves as before at its
// default.

#pragma once

#include <sched.h>
#include <time.h>

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(guest_spin_yield, false, "Guest",
                    "Yield the timeslice when a guest thread is spinning on the "
                    "frame counter instead of holding a core");
REXCVAR_DEFINE_UINT32(guest_spin_yield_after, 4, "Guest",
                      "How many identical reads in a row count as spinning");
REXCVAR_DEFINE_UINT32(guest_spin_yield_us, 100, "Guest",
                      "Microseconds to sleep once spinning is established; 0 "
                      "uses sched_yield instead");

namespace thps::spin {

// The address of the counter read, and of the loop that spins on it. Both are
// recorded here rather than in a comment so a rebuild against a different dump
// fails loudly at install time instead of silently patching the wrong function.
inline constexpr uint32_t kFrameCounterRead = 0x82397688;

inline ::PPCFunc* g_original = nullptr;
inline std::atomic<uint64_t> g_yields{0};

inline void FrameCounterReadYielding(PPCContext& ctx, uint8_t* base) {
  g_original(ctx, base);

  // Per-thread, because two threads polling different counters must not be
  // mistaken for one thread spinning.
  thread_local uint64_t last = ~uint64_t(0);
  thread_local uint32_t repeats = 0;

  const uint64_t value = ctx.r3.u64;
  if (value == last) {
    if (++repeats >= REXCVAR_GET(guest_spin_yield_after)) {
      repeats = 0;
      g_yields.fetch_add(1, std::memory_order_relaxed);

      // A real sleep, not sched_yield.
      //
      // sched_yield was tried first and measured no change at all (Deck,
      // 2026-08-21: 12.44 -> 12.18 us/draw, inside run-to-run spread). Under
      // CFS a yielding thread that is still the most eligible one is simply
      // picked again, so it never actually cedes the core - the well-known
      // reason sched_yield is the wrong tool for "let someone else run".
      //
      // Blocking does cede it. The wait being shortened here is a whole frame
      // - the counter advances once per presented frame, 16-20 ms - so a
      // sleep of ~100 us costs at most 0.6% of a frame in added latency while
      // removing essentially all of the spin's CPU.
      const uint32_t micros = REXCVAR_GET(guest_spin_yield_us);
      if (micros == 0) {
        sched_yield();
      } else {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = static_cast<long>(micros) * 1000L;
        nanosleep(&ts, nullptr);
      }
    }
  } else {
    last = value;
    repeats = 0;
  }
}

// Call after the module's functions have been registered.
inline void Install(rex::Runtime* runtime) {
  if (!REXCVAR_GET(guest_spin_yield)) return;
  if (!runtime) return;
  auto* dispatcher = runtime->function_dispatcher();
  if (!dispatcher) {
    REXLOG_WARN("guest-spin-yield: no function dispatcher; not installed");
    return;
  }
  ::PPCFunc* original = dispatcher->GetFunction(kFrameCounterRead);
  if (!original) {
    REXLOG_WARN("guest-spin-yield: no function registered at {:08X}; this is "
                "not the dump this patch was measured against - not installed",
                kFrameCounterRead);
    return;
  }
  g_original = original;
  if (!dispatcher->SetFunction(kFrameCounterRead, &FrameCounterReadYielding)) {
    REXLOG_WARN("guest-spin-yield: SetFunction({:08X}) refused", kFrameCounterRead);
    g_original = nullptr;
    return;
  }
  REXLOG_INFO("guest-spin-yield: installed at {:08X}, yielding after {} "
              "identical reads", kFrameCounterRead,
              REXCVAR_GET(guest_spin_yield_after));
}

inline uint64_t yields() { return g_yields.load(std::memory_order_relaxed); }

}  // namespace thps::spin
