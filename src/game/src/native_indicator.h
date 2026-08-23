// native_indicator.h - on-screen native-residency mode indicator.
//
// A
// one-line ImGui corner readout of which draw path the frames on screen are
// actually taking - NATIVE-RES / EMULATED / LATCHED - plus the per-frame
// native-versus-fallback draw split from the perf counters. It is the human
// half of the fallback contract: the F5 bind flips the
// mode, this tells you the flip landed, and it is the only way to see the
// sticky auto-disable latch fire without tailing the log.
//
// Wiring (three lines in the app, matching parity_capture.h):
//
//   #include "native_indicator.h"
//   ...
//   void OnCreateDialogs(rex::ui::ImGuiDrawer* d) override {
//     native_indicator::Attach(d, &app_context());
//   }
//   void OnShutdown() override { native_indicator::Detach(); }
//
// and, from the F5 bind, native_indicator::Show() so a run that starts
// emulated still gets an indicator the moment a human asks for the other
// mode.
//
// The F5 BIND ITSELF IS NOT HERE. It lives in thps_p8_app.h and writes the
// cvar through parity_capture::ToggleResidency, which is also what the
// scripted --parity_toggle_schedule path calls; keeping one write path is what
// makes a fixture toggle and a human keypress the same event. This header only
// reads.
//
// ---------------------------------------------------------------- visibility
//
// The indicator is NOT attached by default, and that is a correctness
// requirement, not a taste call. ImGuiDrawer attaches itself to the presenter
// on its first dialog and then calls RequestUIPaintFromUIThread() every frame
// for as long as one exists (imgui_drawer.cpp:65-75, :417-425). An
// always-present dialog would therefore add a per-frame UI repaint to every
// run - including the emulated baselines the M3 gate compares against. So:
//
//   * attached at startup only when the residency cvar is already true, when
//     --parity_toggle_schedule is set (a run that intends to toggle wants the
//     readout from frame 0), or when --native_indicator asks for it outright;
//   * attached on demand by Show(), which the F5 bind calls;
//   * never detached until shutdown - once you have asked for the mode you
//     want to keep seeing EMULATED after you turn it back off.
//
// A run that never enables residency and never presses F5 creates no dialog,
// so the drawer stays detached and the frame is byte-for-byte what it is
// today. Show() is safe from any thread: it hops to the UI thread through
// CallInUIThreadDeferred, because attaching a dialog touches the window's
// input-listener list and the presenter's drawer list.
//
// ------------------------------------------------------------- latch reading
//
// The plugin exposes no latch state. The sticky auto-disable latch
// (the last tier of that contract) is implemented as "clear gpu_native_residency
// and log a warning" on the command-processor thread, so from outside the
// plugin a latch trip and a human turning the mode off are the same cvar
// write. They are told apart by intent: parity_capture publishes the last
// value THIS PROCESS wrote to the cvar (g_residency_last_written), and the
// only other writer is the latch. Intended-on plus actually-off is therefore
// a latch trip, and it is displayed as one until the next write re-arms it -
// which is exactly what an F5 press does, since ToggleResidency reads the
// cleared cvar and writes back true.
//
// Before any write in this process the intent is whatever the cvar held at
// startup, so --gpu_native_residency=true cleared by the latch reads as
// LATCHED too, with no keypress involved.
//
// If the SDK has no residency patches at all the cvar is simply not
// registered; the indicator then reports NO-RESIDENCY and this header still
// compiles and runs, exactly like parity_capture.h's by-name cvar access. The
// perf counters get the same treatment through a `requires` probe on the
// CounterId enumerators, so an unpatched counter.h drops the draw split
// instead of failing to build.

#pragma once

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "parity_capture.h"

REXCVAR_DEFINE_BOOL(native_indicator, false, "Debug",
                    "Show the residency corner indicator from startup, even on "
                    "a run that stays emulated (adds a per-frame UI repaint - "
                    "never set it on a measured run)");

namespace native_indicator {

// The residency master switch, addressed by name for the same reason
// parity_capture does it: the app must build and run against an SDK that has
// never heard of it.
inline constexpr const char* kResidencyCvar = parity_capture::kResidencyCvar;

namespace detail {

enum class Mode {
  kUnavailable,  // the residency cvars are not in this build at all
  kEmulated,     // off, and off on purpose
  kNative,       // on
  kLatched,      // this process last asked for on; something else cleared it
};

// Reads the two draw counters if this SDK has them. Templated on the enum so
// the discarded branch is never instantiated against a counter.h that predates
// the native-residency series - `if constexpr` in a non-template function
// would still have to name the enumerators.
template <typename CounterId = rex::perf::CounterId>
inline bool ReadDrawSplit(int64_t& native_out, int64_t& fallback_out) {
  native_out = 0;
  fallback_out = 0;
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  if constexpr (requires {
                  CounterId::kNativeDraws;
                  CounterId::kNativeFallbackDraws;
                }) {
    // Last-frame snapshot, published by Profiler::Flip() on the command
    // processor thread and read here on the UI thread. Torn-read benign and
    // deliberate: the debug overlay reads every other counter the same way,
    // and a display that is one frame stale is what "per-frame" means here.
    native_out = rex::perf::GetSnapshotCounter(CounterId::kNativeDraws);
    fallback_out = rex::perf::GetSnapshotCounter(CounterId::kNativeFallbackDraws);
    return true;
  }
#endif
  return false;
}

// Whether the mode this process last asked for is on. Falls back to the cvar's
// startup value while this process has written nothing.
inline bool IntendedOn(bool startup_on) {
  int last = parity_capture::g_residency_last_written.load(std::memory_order_acquire);
  return last < 0 ? startup_on : last != 0;
}

inline Mode CurrentMode(bool startup_on) {
  if (!rex::cvar::GetFlagInfo(kResidencyCvar)) {
    return Mode::kUnavailable;
  }
  if (rex::cvar::Query<bool>(kResidencyCvar)) {
    return Mode::kNative;
  }
  return IntendedOn(startup_on) ? Mode::kLatched : Mode::kEmulated;
}

class IndicatorDialog final : public rex::ui::ImGuiDialog {
 public:
  IndicatorDialog(rex::ui::ImGuiDrawer* drawer, bool startup_on)
      : rex::ui::ImGuiDialog(drawer), startup_on_(startup_on) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const Mode mode = CurrentMode(startup_on_);

    const char* label = "EMULATED";
    ImVec4 color(0.78f, 0.78f, 0.78f, 1.0f);
    switch (mode) {
      case Mode::kNative:
        label = "NATIVE-RES";
        color = ImVec4(0.36f, 0.92f, 0.45f, 1.0f);
        break;
      case Mode::kLatched:
        label = "LATCHED";
        color = ImVec4(1.00f, 0.52f, 0.20f, 1.0f);
        break;
      case Mode::kUnavailable:
        label = "NO-RESIDENCY";
        color = ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
        break;
      case Mode::kEmulated:
        break;
    }

    // Fixed buffer, formatted in place: this runs every frame and has no
    // business allocating.
    char detail[96];
    detail[0] = '\0';
    if (mode == Mode::kLatched) {
      std::snprintf(detail, sizeof(detail), "auto-disabled - F5 re-arms");
    } else if (mode == Mode::kNative) {
      int64_t native_draws = 0;
      int64_t fallback_draws = 0;
      if (ReadDrawSplit(native_draws, fallback_draws)) {
        const int64_t total = native_draws + fallback_draws;
        std::snprintf(detail, sizeof(detail), "draws %" PRId64 "  fallback %" PRId64 " (%.1f%%)",
                      total, fallback_draws,
                      total ? 100.0 * double(fallback_draws) / double(total) : 0.0);
      }
    }

    const float margin = 8.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, margin), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("##native_residency_indicator", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::PushStyleColor(ImGuiCol_Text, color);
      ImGui::TextUnformatted(label);
      ImGui::PopStyleColor();
      if (detail[0]) {
        ImGui::SameLine();
        ImGui::TextUnformatted(detail);
      }
    }
    ImGui::End();
  }

 private:
  const bool startup_on_;
};

// One indicator per process. A singleton rather than an app member so the F5
// bind's lambda can stay captureless - thps_p8_app.h documents why that
// matters (a gdb-driven fallback calls it by its mangled name).
struct State {
  rex::ui::ImGuiDrawer* drawer = nullptr;
  rex::ui::WindowedAppContext* context = nullptr;
  std::unique_ptr<IndicatorDialog> dialog;
  bool startup_on = false;
};

inline State& state() {
  static State s;
  return s;
}

// UI thread only: creates the dialog, which is what attaches the ImGui drawer
// to the presenter.
inline void ShowFromUIThread() {
  State& s = state();
  if (s.dialog || !s.drawer) {
    return;
  }
  s.dialog = std::make_unique<IndicatorDialog>(s.drawer, s.startup_on);
}

}  // namespace detail

// Makes the indicator visible from now on. Safe from any thread; the attach
// itself is deferred to the UI thread.
inline void Show() {
  detail::State& s = detail::state();
  if (s.dialog || !s.drawer) {
    return;
  }
  if (s.context && !s.context->IsInUIThread()) {
    s.context->CallInUIThreadDeferred([] { detail::ShowFromUIThread(); });
    return;
  }
  detail::ShowFromUIThread();
}

// Call from OnCreateDialogs. Records the drawer and decides whether this run
// starts with the indicator up; see the visibility note at the top.
inline void Attach(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context) {
  detail::State& s = detail::state();
  // SetupOverlays builds a fresh drawer each time it runs, so never keep a
  // dialog across an attach - it would be pointing at a dead drawer.
  s.dialog.reset();
  s.drawer = drawer;
  s.context = context;
  s.startup_on =
      rex::cvar::GetFlagInfo(kResidencyCvar) && rex::cvar::Query<bool>(kResidencyCvar);
  const bool toggle_run =
      rex::cvar::GetFlagInfo("parity_toggle_schedule") &&
      !rex::cvar::Query<std::string>("parity_toggle_schedule").empty();
  if (s.startup_on || toggle_run || REXCVAR_QUERY(bool, native_indicator)) {
    Show();
  }
}

// Call from OnShutdown, which ReXApp runs before it destroys the drawer
// (rex_app.cpp:549 vs :571).
inline void Detach() {
  detail::State& s = detail::state();
  s.dialog.reset();
  s.drawer = nullptr;
  s.context = nullptr;
}

}  // namespace native_indicator
