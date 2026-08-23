// Deferred guest calls: run host-authored work on a guest thread.
//
// WHY THIS EXISTS
//
// `rex::GuestToHostFunction<T>(fn, args...)` (rex/ppc/function.h) builds a
// PPCContext from the *calling thread's* guest context. It requires
// `rex::runtime::ThreadState::Get()` to be non-null and, when it is null,
// returns a value-initialised T and does nothing else. No log, no assert.
//
// A console command runs on the UI thread, which has no guest thread state. So
// a cheat implemented as a direct call from a console callback does not fail -
// it silently succeeds at doing nothing, which is the single worst failure mode
// available to us. `prove-the-mechanism-engaged` is a standing rule in this
// workspace precisely because "no effect" and "not applied" are indistinguish-
// able after the fact.
//
// So: console commands POST closures here, and a disconnected input driver
// DRAINS them after the title polls its real/scripted pads. Every post is counted and
// every drain is counted, and the two counters are reported by `dev_status`,
// so "the command did nothing" and "the command never ran" are always
// separable.
//
// WHERE IT DRAINS
//
// The SDK's InputSystem calls every registered driver while servicing a guest
// XInput poll. rexglue_script_input.h appends an observer driver which returns
// DEVICE_NOT_CONNECTED after calling Pump(), so it cannot alter merged pad
// state. This is a safe guest thread: the fixture command observer was proven
// there, while the GPU callback thread used by sub_823975B8 hung on nested
// guest calls. Do not move the pump back to the swap-complete producer.
//
// COST WHEN IDLE
//
// Install is gated on `--dev_console`. With the cvar off nothing is hooked at
// all and this header contributes only its statics. With it on, a poll that
// posts nothing costs one relaxed atomic load plus a swap-counter read, the same discipline
// every other guest-behaviour lever in this project follows.

#pragma once

#include <atomic>
#include <cstring>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>
#include <rex/types.h>

namespace thps::guest_call {

using Job = std::function<void()>;

namespace detail {

struct State {
  std::mutex mutex;
  std::deque<Job> queue;
  std::atomic<uint64_t> pending{0};   // cheap "is there work" probe
  std::atomic<uint64_t> posted{0};
  std::atomic<uint64_t> drained{0};
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> last_swap{0};
  std::atomic<bool> saw_swap{false};
  std::atomic<uint64_t> threadless_pumps{0};
  std::atomic<bool> installed{false};
};

inline State& state() {
  static State s;
  return s;
}

// Runs on a guest thread, so ThreadState::Get() is valid here and
// GuestToHostFunction works from inside a job.
inline void ObserveFrame(rex::runtime::ThreadState* thread) {
  constexpr uint32_t kSwapCounter = 0x82970A10;
  const uint8_t* p =
      thread->memory()->TranslateVirtual<const uint8_t*>(kSwapCounter);
  uint64_t raw = 0;
  std::memcpy(&raw, p, sizeof(raw));
  const uint64_t current = rex::byte_swap(raw);

  State& s = state();
  if (!s.saw_swap.exchange(true, std::memory_order_relaxed)) {
    s.last_swap.store(current, std::memory_order_relaxed);
    return;
  }
  const uint64_t previous = s.last_swap.exchange(current, std::memory_order_relaxed);
  if (current > previous) {
    s.frames.fetch_add(current - previous, std::memory_order_relaxed);
  }
}

inline void Drain() {
  State& s = state();
  if (s.pending.load(std::memory_order_relaxed) == 0) return;

  // Swap the whole queue out under the lock and run it unlocked: a job may post
  // another job (a cheat that chains), and running under the lock would
  // deadlock on the re-entrant post.
  std::deque<Job> work;
  {
    std::lock_guard lock(s.mutex);
    work.swap(s.queue);
    s.pending.store(0, std::memory_order_relaxed);
  }
  for (auto& job : work) {
    try {
      job();
    } catch (const std::exception& e) {
      REXLOG_ERROR("guest-call: job threw: {}", e.what());
    } catch (...) {
      REXLOG_ERROR("guest-call: job threw a non-std exception");
    }
    s.drained.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace detail

// Post work to the next guest frame. Safe from any thread, including the UI
// thread and a script-input polling thread.
inline void Post(Job job) {
  detail::State& s = detail::state();
  {
    std::lock_guard lock(s.mutex);
    s.queue.push_back(std::move(job));
    s.pending.store(s.queue.size(), std::memory_order_relaxed);
  }
  s.posted.fetch_add(1, std::memory_order_relaxed);
}

inline bool installed() {
  return detail::state().installed.load(std::memory_order_relaxed);
}
inline uint64_t posted() {
  return detail::state().posted.load(std::memory_order_relaxed);
}
inline uint64_t drained() {
  return detail::state().drained.load(std::memory_order_relaxed);
}
inline uint64_t frames() {
  return detail::state().frames.load(std::memory_order_relaxed);
}
inline uint64_t threadless_pumps() {
  return detail::state().threadless_pumps.load(std::memory_order_relaxed);
}

// Called by the disconnected observer input driver. With the developer surface
// off that driver is not installed, so this path does not exist in normal runs.
inline void Pump() {
  detail::State& s = detail::state();
  if (!s.installed.load(std::memory_order_relaxed)) return;
  auto* thread = rex::runtime::ThreadState::Get();
  if (!thread) {
    const uint64_t misses =
        s.threadless_pumps.fetch_add(1, std::memory_order_relaxed) + 1;
    if (misses == 1) {
      REXLOG_ERROR("guest-call: input-poll pump has no bound guest thread; "
                   "queued calls cannot run");
    }
    return;
  }
  detail::ObserveFrame(thread);
  detail::Drain();
}

inline bool Install() {
  detail::State& s = detail::state();
  if (s.installed.exchange(true, std::memory_order_relaxed)) return true;
  REXLOG_INFO("guest-call: input-poll pump armed");
  return true;
}

}  // namespace thps::guest_call
