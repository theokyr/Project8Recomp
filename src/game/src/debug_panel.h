// debug_panel.h - one F1 panel answering "what is this run doing right now".
//
// Wiring (three lines in the app, matching native_indicator.h):
//
//   #include "debug_panel.h"
//   ...
//   void OnCreateDialogs(rex::ui::ImGuiDrawer* d) override {
//     debug_panel::Attach(d, &app_context());     // registers F1 only
//   }
//   void OnShutdown() override { debug_panel::Detach(); }
//
// What it shows, in the order it is usually wanted:
//
//   PERF       frame time, fps, draw calls, vertices, cache misses. Live from
//              the same per-frame snapshot that writes frames.csv, so what is
//              on screen and what lands in the CSV are the same numbers.
//   REPLAY     the loaded fixture, its marks, and which ones have fired.
//   RECORD     whether a recording is open, where, how much is in it, and the
//              name the next F9 press will stamp.
//   PATH       native-residency mode and the draw split, mirroring the corner
//              indicator so one panel is enough.
//   POST FX    the runtime's own post-processing flags. All restart-gated, so
//              this section builds a relaunch command rather than pretending
//              to drive them live - see DrawPostFx().
//   RUN        where this run's log and CSV are going.
//
// ------------------------------------------------------------------- knobs
//
// Controls are generated from the cvar registry, not hardcoded: FlagEntry
// carries the type, the constraints and - the part that matters - the
// lifecycle. A hot-reload flag becomes a live widget; a restart-gated one
// becomes a staged choice that feeds a relaunch command line. So the panel
// cannot claim a knob works when the build says it does not, and a flag that
// is absent from this build says "absent" instead of silently disappearing.
//
// ---------------------------------------------------------------- visibility
//
// Hidden by default, and that is a measurement requirement rather than a
// preference. ImGuiDrawer attaches itself to the presenter on its first dialog
// and then requests a UI repaint every frame for as long as one exists, so an
// always-present panel would add per-frame UI work to every run - including
// the emulated baselines the perf gates compare against. native_indicator.h
// documents the same constraint at length and solves it the same way:
//
//   * Attach() registers the keybind and NOTHING else - no dialog, no repaint,
//     a run that never presses F1 is byte-for-byte a run without this header;
//   * the first F1 press creates the dialog;
//   * later presses hide and show it by flipping a flag, without destroying it
//     - once the drawer is attached the cost is already paid, and tearing the
//     dialog down and back up would only add churn.
//
// So: never open the panel during a measured run. `make perf-run` and the perf
// gates must stay untouched, which they are as long as nobody presses F1.
//
// ------------------------------------------------------------------ counters
//
// Counter reads go through GetSnapshotCounter - the last-frame values
// published by Profiler::Flip() on the command-processor thread - and are
// read here on the UI thread without synchronization. That is deliberate and
// is what the SDK's own debug overlay does: a torn read of a display counter
// is a number one frame stale, which is what "per-frame" means here anyway.
//
// The native-residency counters are reached through a `requires` probe on the
// enumerators, exactly like native_indicator.h, so this header still builds
// against an SDK that predates the residency patch series.

#pragma once

#include <atomic>
#include <cfloat>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/windowed_app_context.h>

#include "native_indicator.h"
#include "rexglue_input_record.h"
#include "rexglue_script_input.h"

REXCVAR_DEFINE_BOOL(debug_panel, false, "Debug",
                    "Open the debug panel at startup instead of waiting for F1 "
                    "(adds a per-frame UI repaint - never set it on a measured "
                    "run)");

namespace debug_panel {

inline constexpr const char* kBindName = "bind_debug_panel";
inline constexpr const char* kBindKey = "F1";

namespace detail {

// Reads a counter that every SDK in this series has. Returns 0 with counters
// compiled out (Release), which is honest: the CSV is empty in that build too.
inline int64_t Counter(rex::perf::CounterId id) {
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  return rex::perf::GetSnapshotCounter(id);
#else
  (void)id;
  return 0;
#endif
}

// Same `requires` shape as native_indicator::ReadDrawSplit, for the counters
// that only exist once the residency series is applied.
template <typename CounterId = rex::perf::CounterId>
inline bool ReadDrawSplit(int64_t& native_out, int64_t& fallback_out) {
  native_out = 0;
  fallback_out = 0;
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  if constexpr (requires {
                  CounterId::kNativeDraws;
                  CounterId::kNativeFallbackDraws;
                }) {
    native_out = rex::perf::GetSnapshotCounter(CounterId::kNativeDraws);
    fallback_out = rex::perf::GetSnapshotCounter(CounterId::kNativeFallbackDraws);
    return true;
  }
#endif
  return false;
}

// A cvar this build may or may not have, rendered for display.
inline std::string CvarText(const char* name) {
  if (!rex::cvar::GetFlagInfo(name)) return "(absent)";
  return rex::cvar::Query<bool>(name) ? "true" : "false";
}

inline void Row(const char* label, const char* value) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::TextUnformatted(value);
}

// A path, shown short. The question this panel answers about a path is "which
// run is this", and the last couple of components answer it; the full string
// goes in a tooltip so nothing is lost and nothing is wide. Absolute run paths
// are the single biggest thing that used to stretch the window.
inline void RowPath(const char* label, const std::string& value, const char* if_empty) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  if (value.empty()) {
    ImGui::TextDisabled("%s", if_empty);
    return;
  }
  std::string shown = value;
  const std::size_t last = value.find_last_of('/');
  if (last != std::string::npos && last > 0) {
    const std::size_t prev = value.find_last_of('/', last - 1);
    if (prev != std::string::npos) {
      shown = "..." + value.substr(prev);
    }
  }
  ImGui::TextUnformatted(shown.c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", value.c_str());
  }
}

// --------------------------------------------------------------- cvar knobs
//
// One control per registered cvar, with every property read from the registry
// rather than hardcoded here: type picks the widget, constraints pick the
// range or the dropdown entries, and LIFECYCLE decides whether it is a control
// at all.
//
// That last one is the point. Half the interesting graphics flags are
// `kRequiresRestart` - writing them succeeds and then does nothing until the
// next launch - and a slider that silently does nothing is worse than no
// slider. Those render read-only with a tag, and RestartFlags() collects them
// into a command line to relaunch with. Nothing in this file asserts which
// flags those are; the registry is asked, so the panel cannot drift from the
// build it is compiled into.
//
// Values move as strings through the registry's own getter/setter, so no
// typed Query instantiation is needed and a flag type this panel has no widget
// for still displays correctly.

inline bool CvarIsLive(const rex::cvar::FlagEntry& entry) {
  return entry.lifecycle == rex::cvar::Lifecycle::kHotReload;
}

inline const char* LifecycleTag(const rex::cvar::FlagEntry& entry) {
  switch (entry.lifecycle) {
    case rex::cvar::Lifecycle::kHotReload:
      return "";
    case rex::cvar::Lifecycle::kRequiresRestart:
      return "restart";
    case rex::cvar::Lifecycle::kInitOnly:
      return "init only";
  }
  return "";
}

inline std::string CvarValue(const rex::cvar::FlagEntry& entry) {
  return entry.getter ? entry.getter() : std::string();
}

// Draws the control for one cvar into the current table cell. Returns true if
// the value was changed this frame.
inline bool CvarControl(const rex::cvar::FlagEntry& entry) {
  const std::string id = "##cvar_" + entry.name;
  const std::string current = CvarValue(entry);
  bool changed = false;

  if (!CvarIsLive(entry)) {
    // Read-only on purpose: see the note above. The value is still the live
    // one, so this stays a truthful readout of what the run is doing.
    ImGui::TextUnformatted(current.empty() ? "(unset)" : current.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", LifecycleTag(entry));
    return false;
  }

  ImGui::SetNextItemWidth(-FLT_MIN);
  switch (entry.type) {
    case rex::cvar::FlagType::Boolean: {
      bool value = (current == "true" || current == "1");
      if (ImGui::Checkbox(id.c_str(), &value)) {
        changed = rex::cvar::SetFlagByName(entry.name, value ? "true" : "false");
      }
      break;
    }
    case rex::cvar::FlagType::Double: {
      float value = std::strtof(current.c_str(), nullptr);
      const bool ranged = entry.constraints.HasRangeConstraint();
      const float lo = float(entry.constraints.min.value_or(0.0));
      const float hi = float(entry.constraints.max.value_or(1.0));
      const bool edited = ranged
                              ? ImGui::SliderFloat(id.c_str(), &value, lo, hi, "%.3f")
                              : ImGui::DragFloat(id.c_str(), &value, 0.01f, 0.0f, 0.0f, "%.3f");
      if (edited) {
        char text[64];
        std::snprintf(text, sizeof(text), "%g", double(value));
        changed = rex::cvar::SetFlagByName(entry.name, text);
      }
      break;
    }
    case rex::cvar::FlagType::Int32:
    case rex::cvar::FlagType::Int64:
    case rex::cvar::FlagType::Uint32:
    case rex::cvar::FlagType::Uint64: {
      int value = int(std::strtol(current.c_str(), nullptr, 10));
      const bool ranged = entry.constraints.HasRangeConstraint();
      const int lo = int(entry.constraints.min.value_or(0.0));
      const int hi = int(entry.constraints.max.value_or(0.0));
      const bool edited = ranged ? ImGui::SliderInt(id.c_str(), &value, lo, hi)
                                 : ImGui::DragInt(id.c_str(), &value, 1.0f);
      if (edited) {
        char text[64];
        std::snprintf(text, sizeof(text), "%d", value);
        changed = rex::cvar::SetFlagByName(entry.name, text);
      }
      break;
    }
    case rex::cvar::FlagType::String: {
      if (entry.constraints.HasAllowedValues()) {
        // A closed set is a dropdown; free strings are not editable here,
        // because a half-typed path applied per keystroke is a footgun.
        const auto& values = entry.constraints.allowed_values;
        if (ImGui::BeginCombo(id.c_str(), current.c_str())) {
          for (const std::string& option : values) {
            const bool selected = (option == current);
            if (ImGui::Selectable(option.c_str(), selected)) {
              changed = rex::cvar::SetFlagByName(entry.name, option);
            }
            if (selected) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
      } else {
        ImGui::TextUnformatted(current.empty() ? "(unset)" : current.c_str());
      }
      break;
    }
    default:
      ImGui::TextUnformatted(current.empty() ? "(unset)" : current.c_str());
      break;
  }
  return changed;
}

// A restart-gated flag cannot be changed in this run, but it CAN be chosen for
// the next one. This renders the same widget bound to a local staging map
// instead of the cvar, so the panel becomes a launch-argument builder for the
// flags it cannot drive live. Nothing is written to the registry here - the
// staged value only ever leaves through the relaunch command line.
inline void CvarStagedControl(const rex::cvar::FlagEntry& entry,
                              std::map<std::string, std::string>& staged) {
  const std::string current = CvarValue(entry);
  auto it = staged.find(entry.name);
  const std::string& chosen = (it == staged.end()) ? current : it->second;
  const std::string id = "##staged_" + entry.name;

  ImGui::SetNextItemWidth(-FLT_MIN);
  if (entry.type == rex::cvar::FlagType::String &&
      entry.constraints.HasAllowedValues()) {
    if (ImGui::BeginCombo(id.c_str(), chosen.c_str())) {
      for (const std::string& option : entry.constraints.allowed_values) {
        const bool selected = (option == chosen);
        if (ImGui::Selectable(option.c_str(), selected)) staged[entry.name] = option;
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  } else if (entry.type == rex::cvar::FlagType::Boolean) {
    bool value = (chosen == "true" || chosen == "1");
    if (ImGui::Checkbox(id.c_str(), &value)) {
      staged[entry.name] = value ? "true" : "false";
    }
  } else if (entry.type == rex::cvar::FlagType::Double) {
    float value = std::strtof(chosen.c_str(), nullptr);
    const bool ranged = entry.constraints.HasRangeConstraint();
    const bool edited =
        ranged ? ImGui::SliderFloat(id.c_str(), &value,
                                    float(entry.constraints.min.value_or(0.0)),
                                    float(entry.constraints.max.value_or(1.0)), "%.3f")
               : ImGui::DragFloat(id.c_str(), &value, 0.01f, 0.0f, 0.0f, "%.3f");
    if (edited) {
      char text[64];
      std::snprintf(text, sizeof(text), "%g", double(value));
      staged[entry.name] = text;
    }
  } else {
    ImGui::TextUnformatted(chosen.empty() ? "(unset)" : chosen.c_str());
  }

  if (chosen != current) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.30f, 1.0f), "next launch");
  } else {
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", LifecycleTag(entry));
  }
}

// One table row: label on the left, control on the right, the cvar's own
// description as the tooltip. Absent flags say so rather than vanishing - "this
// build does not have it" is information too.
inline bool CvarRow(const char* label, const char* name) {
  const rex::cvar::FlagEntry* entry = rex::cvar::GetFlagInfo(name);
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  if (entry && !entry->description.empty() &&
      ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s\n\n--%s", entry->description.c_str(), entry->name.c_str());
  }
  ImGui::TableSetColumnIndex(1);
  if (!entry) {
    ImGui::TextDisabled("(absent in this build)");
    return false;
  }
  return CvarControl(*entry);
}

// Same row, but a restart-gated flag becomes a staged choice rather than a
// read-only readout. Live flags behave exactly as in CvarRow.
inline void CvarRowStaged(const char* label, const char* name,
                          std::map<std::string, std::string>& staged) {
  const rex::cvar::FlagEntry* entry = rex::cvar::GetFlagInfo(name);
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  if (entry && !entry->description.empty() &&
      ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s\n\n--%s", entry->description.c_str(), entry->name.c_str());
  }
  ImGui::TableSetColumnIndex(1);
  if (!entry) {
    ImGui::TextDisabled("(absent in this build)");
    return;
  }
  if (CvarIsLive(*entry)) {
    CvarControl(*entry);
    return;
  }
  CvarStagedControl(*entry, staged);
}

class PanelDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit PanelDialog(rex::ui::ImGuiDrawer* drawer)
      : rex::ui::ImGuiDialog(drawer) {}

  void set_visible(bool visible) { visible_ = visible; }
  bool visible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!visible_) return;

    // Sized, not auto-sized. AlwaysAutoResize grows the window to the widest
    // thing in it, and this panel prints run paths and cvar values - one long
    // --log_file was enough to cover the screen. A fixed default with a hard
    // max, and the user can drag it wider if they want to.
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(kPanelWidth, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f, 0.0f),
                                        ImVec2(kPanelMaxWidth, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.82f);
    if (!ImGui::Begin("thps_p8  (F1)", nullptr,
                      ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing)) {
      ImGui::End();
      return;
    }
    (void)io;

    DrawPerf();
    DrawReplay();
    DrawRecord();
    DrawPath();
    DrawPostFx();
    DrawRun();

    ImGui::End();
  }

 private:
  // Scratch for the whole frame's formatting. One buffer, reused per row: this
  // runs every frame the panel is open and has no business allocating.
  char buf_[192];

  const char* Fmt(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf_, sizeof(buf_), fmt, args);
    va_end(args);
    return buf_;
  }

  bool Section(const char* title) {
    return ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen);
  }

  // Label column fixed, value column takes the rest. With StretchProp both
  // columns grew with their content, which is half of why the panel used to
  // run away; this way a long value is clipped by the window instead of
  // widening it.
  bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2,
                           ImGuiTableFlags_SizingFixedFit |
                               ImGuiTableFlags_NoSavedSettings)) {
      return false;
    }
    ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, kLabelWidth);
    ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
    return true;
  }

  void DrawPerf() {
    if (!Section("PERF")) return;
#ifndef REXGLUE_ENABLE_PERF_COUNTERS
    ImGui::TextUnformatted("counters compiled out (Release build)");
    return;
#endif
    const int64_t frame_us = Counter(rex::perf::CounterId::kFrameTimeUs);
    const int64_t draws = Counter(rex::perf::CounterId::kDrawCalls);
    if (BeginRows("##perf")) {
      Row("frame", Fmt("%.2f ms  (%.1f fps)", double(frame_us) / 1000.0,
                       frame_us > 0 ? 1e6 / double(frame_us) : 0.0));
      Row("draw calls", Fmt("%" PRId64, draws));
      Row("vertices", Fmt("%" PRId64,
                          Counter(rex::perf::CounterId::kVerticesProcessed)));
      Row("pipeline miss", Fmt("%" PRId64,
                               Counter(rex::perf::CounterId::kPipelineCacheMisses)));
      Row("texture miss", Fmt("%" PRId64,
                              Counter(rex::perf::CounterId::kTextureCacheMisses)));
      ImGui::EndTable();
    }
    // The same rule phase_report.py classifies frames.csv rows with, shown
    // live so the threshold can be sanity-checked by eye against the screen
    // instead of only after the run: if this says "front end" while a skate
    // park is on screen, kDraws3d is wrong. Note that "front end" is not
    // "2D" - this title's menus and attract cinematic render real 3D, at a
    // steady ~200 draws, which is exactly why the threshold sits where it does.
    ImGui::TextDisabled("%s", draws >= kDraws3d ? "content: gameplay"
                              : draws > 0       ? "content: front end"
                                                : "content: blank");
  }

  void DrawReplay() {
    const auto status = rexglue_script_input::GetStatus();
    if (!status.loaded) return;  // not a replay run - the section would be noise
    if (!Section("REPLAY")) return;
    if (BeginRows("##replay")) {
      RowPath("source", status.source, "(none)");
      Row("loaded", Fmt("%zu actions, %zu samples, %.1f s",
                        status.actions, status.samples,
                        double(status.end_ms) / 1000.0));
      const size_t total = status.marks ? status.marks->size() : 0;
      Row("marks", Fmt("%zu of %zu fired", status.marks_fired, total));
      ImGui::EndTable();
    }
    if (!status.marks) return;
    for (size_t i = 0; i < status.marks->size(); ++i) {
      const auto& mark = (*status.marks)[i];
      const bool fired = i < status.marks_fired;
      // Fired marks are the run's own history; pending ones are the schedule.
      if (fired) {
        ImGui::TextColored(ImVec4(0.36f, 0.92f, 0.45f, 1.0f), "  %7.1fs  %s",
                           double(mark.t_ms) / 1000.0, mark.name.c_str());
      } else {
        ImGui::TextDisabled("  %7.1fs  %s", double(mark.t_ms) / 1000.0,
                            mark.name.c_str());
      }
    }
  }

  void DrawRecord() {
    auto& writer = rexglue_input_record::Instance();
    const bool recording = writer.is_open();
    if (!Section("RECORD")) return;
    if (BeginRows("##record")) {
      Row("state", recording ? "RECORDING" : "not recording");
      if (recording) {
        RowPath("file", writer.path(), "(none)");
        Row("written", Fmt("%llu samples, %llu mark(s)",
                           (unsigned long long)writer.sample_count(),
                           (unsigned long long)writer.mark_count()));
      }
      Row("F9 stamps", rexglue_input_record::PeekNextMarkName().c_str());
      ImGui::EndTable();
    }
    if (!recording) {
      ImGui::TextDisabled("  start one with `make record`");
    }
  }

  void DrawPath() {
    if (!Section("PATH")) return;
    int64_t native = 0, fallback = 0;
    const bool have_split = ReadDrawSplit(native, fallback);
    if (BeginRows("##path")) {
      // Residency stays a readout, not a checkbox. Its write path has a second
      // job - parity_capture::ToggleResidency records what THIS process asked
      // for, which is how native_indicator tells "the user turned it off" from
      // "the plugin's auto-disable latch tripped". A raw SetFlagByName here
      // would flip the mode and silently corrupt that distinction. F5 is the
      // toggle, and it is the same event a scheduled fixture toggle fires.
      Row("residency", CvarText(native_indicator::kResidencyCvar).c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("[F5]");
      CvarRow("cp fastpath", "gpu_cp_fastpath");
      if (have_split) {
        const int64_t total = native + fallback;
        Row("native draws", Fmt("%" PRId64 " of %" PRId64 "  (fallback %.1f%%)",
                                native, total,
                                total ? 100.0 * double(fallback) / double(total)
                                      : 0.0));
      }
      ImGui::EndTable();
    }
  }

  // First step at post-processing knobs (the three cheapest levers surveyed
  // - the ones that need no code because the runtime already ships them).
  //
  // Every one of these is `kRequiresRestart` in the SDK, so none of them can be
  // A/B'd inside one run. Rather than pretend otherwise, the section is a
  // launch-argument builder: pick what you want, copy the line, relaunch. The
  // staged value is never written to the registry.
  //
  // Items 4-8 of that survey - post-processing off, bloom off, brightness and
  // bloom-intensity sliders - are the ones worth having live, and every one of
  // them needs an SDK patch (a hash predicate in IssueDraw, or a constant
  // override keyed on a pixel-shader hash). This section is deliberately the
  // no-patch half; it exists so the plumbing and the panel real estate are
  // there when those land, at which point they become live rows above this
  // note rather than staged ones.
  void DrawPostFx() {
    if (!Section("POST FX")) return;

    // FidelityFX is a compile-time option in the SDK. Rather than assume, ask
    // the registry what `present_effect` will actually accept in THIS build.
    const rex::cvar::FlagEntry* present = rex::cvar::GetFlagInfo("present_effect");
    bool has_ffx = false;
    if (present) {
      for (const std::string& option : present->constraints.allowed_values) {
        if (option == "cas" || option == "fsr" || option == "fsr2" || option == "fsr3") {
          has_ffx = true;
          break;
        }
      }
    }

    if (BeginRows("##postfx")) {
      CvarRowStaged("post AA", "swap_post_effect", staged_);
      CvarRowStaged("present", "present_effect", staged_);
      CvarRowStaged("cas sharpness", "present_cas_additional_sharpness", staged_);
      CvarRowStaged("dither", "present_dither", staged_);
      CvarRowStaged("render scale x", "draw_resolution_scale_x", staged_);
      CvarRowStaged("render scale y", "draw_resolution_scale_y", staged_);
      ImGui::EndTable();
    }

    if (present && !has_ffx) {
      ImGui::TextDisabled("  FidelityFX not compiled into this build - cas/fsr unavailable");
    }

    // Only flags whose staged value differs from what this run is using.
    std::string args;
    for (const auto& [name, value] : staged_) {
      const rex::cvar::FlagEntry* entry = rex::cvar::GetFlagInfo(name.c_str());
      if (!entry || CvarValue(*entry) == value) continue;
      if (!args.empty()) args += ' ';
      args += "--" + name + "=" + value;
    }
    if (args.empty()) {
      ImGui::TextDisabled("  all knobs match this run; nothing to relaunch for");
      return;
    }
    const std::string line = "make play EXTRA='" + args + "'";
    ImGui::TextWrapped("%s", line.c_str());
    if (ImGui::Button("copy relaunch command")) {
      ImGui::SetClipboardText(line.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("reset")) {
      staged_.clear();
    }
  }

  void DrawRun() {
    if (!Section("RUN")) return;
    if (BeginRows("##run")) {
      const std::string csv = rex::cvar::GetFlagInfo("perf_log_csv")
                                  ? rex::cvar::Query<std::string>("perf_log_csv")
                                  : std::string();
      RowPath("perf csv", csv, "(none - not a measured run)");
      const std::string log = rex::cvar::GetFlagInfo("log_file")
                                  ? rex::cvar::Query<std::string>("log_file")
                                  : std::string();
      RowPath("log", log, "(stdout)");
      // Worth a real control rather than a readout: vsync is the one knob shown
      // to decide whether in-level cutscenes run at the right speed
      // (notes/cutscene-rate.md).
      CvarRow("vsync", "vsync");
      ImGui::EndTable();
    }
  }

  // Kept in step with phase_report.py DEFAULT_DRAWS_3D, which carries the
  // measurement this came from. See DrawPerf().
  static constexpr int64_t kDraws3d = 300;

  // Panel geometry. The max width is the load-bearing one: without it a
  // long path or cvar value grows the window without limit.
  static constexpr float kPanelWidth = 420.0f;
  static constexpr float kPanelMaxWidth = 620.0f;
  static constexpr float kLabelWidth = 108.0f;

  // Restart-gated choices for the next launch. Panel-local by design: writing
  // these to the registry would look like it had done something.
  std::map<std::string, std::string> staged_;

  bool visible_ = true;
};

struct State {
  rex::ui::ImGuiDrawer* drawer = nullptr;
  rex::ui::WindowedAppContext* context = nullptr;
  std::unique_ptr<PanelDialog> dialog;
};

inline State& state() {
  static State s;
  return s;
}

// UI thread only. Creating the dialog is what attaches the drawer to the
// presenter, so this is the moment the per-frame UI repaint starts.
inline void ShowFromUIThread(const char* why) {
  State& s = state();
  if (!s.drawer) {
    REXLOG_WARN("debug-panel: asked to open ({}) before the overlay came up; "
                "ignored", why);
    return;
  }
  if (!s.dialog) {
    s.dialog = std::make_unique<PanelDialog>(s.drawer);
    REXLOG_INFO("debug-panel: open ({}). This adds a per-frame UI repaint - "
                "do not leave it open during a measured run.", why);
    return;
  }
  s.dialog->set_visible(true);
}

inline void ToggleFromUIThread() {
  State& s = state();
  if (!s.dialog) {
    ShowFromUIThread("F1");
    return;
  }
  s.dialog->set_visible(!s.dialog->visible());
}

}  // namespace detail

// Safe from any thread; the dialog work hops to the UI thread, because
// attaching one touches the window's input-listener list and the presenter's
// drawer list.
inline void Toggle() {
  detail::State& s = detail::state();
  if (s.context && !s.context->IsInUIThread()) {
    s.context->CallInUIThreadDeferred([] { detail::ToggleFromUIThread(); });
    return;
  }
  detail::ToggleFromUIThread();
}

// Opens the panel from anywhere. Idempotent.
inline void Show(const char* why = "requested") {
  detail::State& s = detail::state();
  if (s.context && !s.context->IsInUIThread()) {
    // The string has to outlive the hop, so only literals may be passed here -
    // every caller passes one.
    s.context->CallInUIThreadDeferred(
        [why] { detail::ShowFromUIThread(why); });
    return;
  }
  detail::ShowFromUIThread(why);
}

// Call from OnCreateDialogs. Registers the key and records the drawer. Creates
// no dialog unless --debug_panel was given: see the visibility note at the top
// of this header for why the default has to stay off.
inline void Attach(rex::ui::ImGuiDrawer* drawer,
                   rex::ui::WindowedAppContext* context) {
  detail::State& s = detail::state();
  // SetupOverlays builds a fresh drawer each time it runs, so a dialog must
  // never outlive an attach - it would point at a dead drawer.
  s.dialog.reset();
  s.drawer = drawer;
  s.context = context;
  rex::ui::RegisterBind(kBindName, kBindKey, "Toggle the thps_p8 debug panel",
                        [] { Toggle(); });
  if (REXCVAR_QUERY(bool, debug_panel)) {
    Show("--debug_panel");
  }
}

// Call from OnShutdown, which ReXApp runs before it destroys the drawer.
inline void Detach() {
  detail::State& s = detail::state();
  s.dialog.reset();
  s.drawer = nullptr;
  s.context = nullptr;
  rex::ui::UnregisterBind(kBindName);
}

}  // namespace debug_panel
