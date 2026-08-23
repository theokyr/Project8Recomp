// thps_p8 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/ui/keybinds.h>

#ifdef __linux__
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif

// Present only in -fprofile-generate builds; weak so normal builds link.
extern "C" int __llvm_profile_write_file(void) __attribute__((weak));
#endif

#include "console_input_guard.h"
#include "debug_panel.h"
#include "dev_cheats.h"
#include "dev_console.h"
#include "dump_gate.h"
#include "guest_call.h"
#include "guest_probe.h"
#include "guest_spin_yield.h"
#include "native_indicator.h"
#include "parity_capture.h"
#include "rexglue_input_record.h"
#include "rexglue_script_input.h"

// A screenshot at every mark of a marked fixture. Off by default because a
// guest-output readback stalls the GPU, and the fixtures that carry marks are
// the same ones `make perf-run` measures - a measured run must not pay for a
// diagnostic. `make replay` and `make record` turn it on (SHOTS=false opts
// out); everything else has to ask.
REXCVAR_DEFINE_BOOL(mark_screenshot, false, "Input",
                    "Capture the guest output into <run dir>/marks/ every time a "
                    "replay mark fires or F9 stamps one");

#ifdef __linux__
// The SDK installs no SIGTERM handler, so the default action kills the
// process before any atexit work runs - which means an instrumented build
// never writes its .profraw and PGO is impossible. Flush and leave.
//
// An input recording has no userspace buffer to lose (see
// rexglue_input_record.h), so all this owes it is an fsync, which is
// async-signal-safe; the run's last line is already in the kernel. That makes
// closing the window, `make stop` and a stray Ctrl-C all leave a parseable
// recording behind.
inline void ThpsP8OnTerminate(int) {
  rexglue_input_record::FlushOnSignal();
  if (__llvm_profile_write_file) {
    __llvm_profile_write_file();
  }
  _exit(0);
}
#endif

class ThpsP8App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<ThpsP8App>(new ThpsP8App(ctx, "thps_p8",
        PPCImageConfig));
  }

  // Scripted input playback: --input_json / --input_json_file drive a
  // virtual pad on slot 0 directly inside the process - no OS injection.
  // Recording (--input_record_file) installs second on purpose: its tap nests
  // whatever the script layer built, so a recording captures the scripted pad
  // as well as the real one and a fixture run can be recorded like a human
  // session. Both are inert with their cvars unset.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    rexglue_script_input::Install(
        config, REXCVAR_GET(dev_console) ? rexglue_script_input::PollObserver{
                                              [] { thps::guest_call::Pump(); }}
                                        : rexglue_script_input::PollObserver{});
    rexglue_input_record::Install(config);
  }

  // The runtime compiles the per-frame perf-counter CSV writer in
  // (Profiler::Flip() pumps it from the command processor), but nothing in
  // SDK v0.8.0 consumes the --perf_log_csv cvar, so the file is never
  // opened. Wire it here once logging is up.
  void OnPostSetup() override {
    // After registration, so GetFunction can find what it is replacing.
    thps::spin::Install(runtime());

#ifdef __linux__
    // Let same-user tools (gdb PC sampler) attach under yama ptrace_scope=1.
    prctl(PR_SET_PTRACER, (unsigned long)-1, 0, 0, 0);
    std::signal(SIGTERM, ThpsP8OnTerminate);
    std::signal(SIGINT, ThpsP8OnTerminate);
#endif
    std::string csv_path = REXCVAR_QUERY(std::string, perf_log_csv);
    if (!csv_path.empty()) {
      rex::perf::SetCsvLogPath(csv_path);
      // Anchor the two clocks to each other. frames.csv carries no timestamp
      // column - its only time axis is the row index, and integrating
      // frame_time_us gives seconds from the FIRST FLIP, not from launch.
      // Input marks are on the input timebase. Neither can be converted to the
      // other without a common point, and this line is it: the input-clock
      // reading at the moment the CSV was armed, which is before any row
      // exists. phase_report.py reads it as the lower bound on row 0.
      REXLOG_INFO("perf: csv armed at input t={}ms -> {}",
                  rexglue_script_input::NowMs(), csv_path);
    }
    // Guest-output parity captures and scheduled residency toggles. Idle
    // unless --parity_capture_frames or --parity_toggle_schedule is given, or
    // F10 is pressed: no thread, no polling, no guest memory reads.
    parity_capture_.Start(runtime());
    InstallMarkScreenshots();

    // The developer console layer. Entirely inert unless --dev_console is set:
    // no guest function is hooked, no launch command runs, and the run is
    // byte-identical to one built without these headers. That is the standing
    // rule for anything that can touch guest behaviour in this project, and it
    // is what keeps the existing perf gates valid across this change.
    if (REXCVAR_GET(dev_console)) {
      thps::probe::SetRuntime(runtime());
      // OnPreSetup appended a disconnected driver to the title's input system.
      // Enabling the queue here lets that known guest input thread drain jobs.
      thps::guest_call::Install();
      thps::cheats::Install();
      // Fixture commands use the exact same parser and dispatcher as typed,
      // --exec and +argument commands. The callback runs on an input-polling
      // guest thread, but ExecuteLine only performs bounded host work and all
      // guest calls still go through the input-poll pump.
      rexglue_script_input::SetCommandObserver(
          [](size_t, const std::string& command, int64_t) {
            thps::dev_console::ExecuteLine(command);
          });
      if (REXCVAR_GET(cheat_skip_intro)) thps::cheats::StartIntroSkipWatch();
      // Ordered after the pump so a `+wait`-bearing launch line has something
      // to defer onto.
      thps::dev_console::RunLaunchCommands();
      thps::dev_console::RunStartupExec();
    }
  }

  // Runs during SetupPresentation, before the runtime exists - registration
  // only; the bind callback resolves the presenter when it actually fires.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    parity_capture_.RegisterBinds();
    RegisterNativeResidencyBind();
    RegisterInputMarkBind();
    // Corner readout of the draw path the frames on screen are actually
    // taking. Creates no dialog - and so leaves the ImGui drawer detached and
    // the frame untouched - unless this run starts in native mode, is a
    // scheduled-toggle run, or someone presses F5. See native_indicator.h.
    native_indicator::Attach(drawer, &app_context());
    // F1: the run's own readout - perf counters, replay marks, recording
    // state, draw path. Registers the key and nothing else; no dialog exists
    // and no UI repaint happens until someone presses it, which is what keeps
    // a measured run identical to one built without this header.
    debug_panel::Attach(drawer, &app_context());
    // Keeps console typing out of the guest pad. The SDK's own suppression
    // keys off WantCaptureMouse, so without this every character typed into
    // the console also drives the skater - see console_input_guard.h.
    if (REXCVAR_GET(dev_console)) thps::console_guard::Attach(window(), drawer);
  }

  void OnShutdown() override {
    // First, because the observer holds `this` and fires from guest threads
    // that are still polling: clearing it is what makes the teardown below
    // safe. Everything after this point can assume no new capture is queued.
    rexglue_script_input::SetMarkObserver({});
    rexglue_script_input::SetCommandObserver({});
    // Ordered: the indicator's dialog has to be gone before ReXApp destroys
    // the drawer it registered itself with, which happens after this returns.
    thps::console_guard::Detach();
    native_indicator::Detach();
    debug_panel::Detach();
    rex::ui::UnregisterBind(kNativeResidencyBind);
    rex::ui::UnregisterBind(kInputMarkBind);
    parity_capture_.Stop();
  }

  // The last hook before the Runtime is constructed, which is what makes a
  // refusal here clean: there is nothing half-built to tear down, and teardown
  // is exactly where the unguarded case aborts. See dump_gate.h for why this
  // exists when the runtime already detects the problem, and why there is
  // deliberately no flag to switch it off.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    dump_gate::Enforce(paths.game_data_root);
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}

 private:
  // Marks name the moments a human already decided were worth looking at, so
  // they are also where a run should be photographed. This is the whole of the
  // wiring: the input layer publishes the event, the capture harness owns the
  // presenter readback, and neither knows about the other.
  //
  // The label carries the mark's ordinal, zero-padded, so `ls` sorts the marks
  // directory into fixture order and two runs of one fixture produce
  // file names that differ only in the mode suffix - which is what makes
  // `mark_03_*_emu.png` vs `mark_03_*_native.png` a comparison rather than a
  // scavenger hunt.
  //
  // The observer runs on a guest input-polling thread. RequestNamedCapture only
  // queues, so nothing here touches the presenter or the filesystem on that
  // thread.
  void InstallMarkScreenshots() {
    if (!REXCVAR_QUERY(bool, mark_screenshot)) {
      return;
    }
    REXLOG_INFO("mark-screenshot: on - one guest-output capture per mark");
    rexglue_script_input::SetMarkObserver(
        [this](size_t ordinal, const std::string& name, int64_t t_ms) {
          parity_capture_.RequestNamedCapture(
              fmt::format("{:02}_{}", ordinal, name), t_ms);
        });
  }

  // Tier 2 of the fallback contract (native-renderer/design.md section 4):
  // F5 flips the master residency cvar mid-run, so one run can produce the
  // toggle pairs the parity protocol grades against. The cvar is latched once
  // per frame at IssueSwap on the CP thread inside rexgpu-xenos - this bind
  // only writes the cvar and never latches or invalidates anything itself.
  //
  // The write itself lives in parity_capture::ToggleResidency, which addresses
  // the cvar by name (exactly like parity_capture's ModeTag(): the app must
  // still build and run against an SDK without the residency patches, where
  // this cvar simply does not exist). With its default target,
  // --parity_toggle_schedule fires the same function from the capture harness's
  // worker thread, so a scripted residency toggle and a human F5 press are
  // byte-for-byte the same event. --parity_toggle_cvar may instead name another
  // boolean cvar for an isolated sub-lever gate. This matters because synthetic
  // keypresses cannot reach the F5 bind at all on an unfocused window
  // (m2-results.md "Anomalies"). The bind stays for humans.
  //
  // The lambda is deliberately captureless: a gdb-driven fallback calls it by
  // its mangled name when neither a keypress nor a schedule is available. The
  // indicator call keeps it that way (native_indicator holds its state in a
  // process singleton for exactly this reason) and is safe off the UI thread,
  // which is where that gdb fallback would land.
  static void RegisterNativeResidencyBind() {
    rex::ui::RegisterBind(kNativeResidencyBind, "F5", "Toggle native residency", [] {
      parity_capture::ToggleResidency("F5");
      // Asking for the other mode is the signal that you want to watch it.
      native_indicator::Show();
    });
  }

  // F9 stamps the next name from --input_mark_names into the recording that
  // `make record` has open, on the input timebase. It is the cheap half of
  // answering "when did this run actually reach gameplay": press it when you
  // see the boundary, and every replay of that take re-announces it at the
  // same millisecond for phase_report.py to line up against frames.csv.
  //
  // Registered unconditionally, like F5 and F10 - the recorder is what knows
  // whether a recording is open, and it warns rather than going quiet when
  // one is not, so a press during a plain `make play` says why nothing
  // happened instead of looking like a dead key.
  static void RegisterInputMarkBind() {
    rex::ui::RegisterBind(kInputMarkBind, "F9", "Mark this moment in the input recording",
                          [] { rexglue_input_record::MarkNext(); });
  }

  static constexpr const char* kNativeResidencyBind = "bind_native_residency";
  static constexpr const char* kNativeResidencyCvar = parity_capture::kResidencyCvar;
  static constexpr const char* kInputMarkBind = "bind_input_mark";

  parity_capture::ParityCapture parity_capture_;
};
