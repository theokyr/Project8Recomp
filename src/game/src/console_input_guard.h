// Stop console typing from reaching the skater.
//
// THE BUG THIS WORKS AROUND
//
// ReXApp suppresses guest input while an overlay is up, but the predicate it
// uses is the wrong one (src/ui/rex_app.cpp:250-259):
//
//     input_sys->SetActiveCallback([this]() {
//       if (!debug_overlay_ && !console_overlay_ && ...) return true;
//       return !imgui_drawer_->GetIO().WantCaptureMouse;
//     });
//
// `WantCaptureMouse` is true only while the cursor is physically over an ImGui
// window. Typing is a keyboard activity, so with the pointer anywhere else -
// which is where it naturally sits while both hands are on the keys - every
// character typed into the console is ALSO delivered to `MnkInputDriver` and
// therefore to the guest pad. This workspace always runs `--mnk_mode=true`
// (the Makefile forces it; the manifest calls it required), so in practice
// typing `cheat_warp 100 0 250` in Free Skate makes the skater jump, grind and
// bail while you type it.
//
// That is not a cosmetic annoyance for this project: a console whose use
// perturbs the game state cannot be used to set up a benchmark.
//
// HOW THIS FIXES IT
//
// Window input events propagate highest-z-order-first and stop at the first
// listener that marks the event handled (src/ui/window.cpp:738-777). The
// ImGuiDrawer sits at z-order 64; ReXApp's keybind handler and MnkInputDriver
// both sit at 0. Registering here at 32 puts us strictly between them: ImGui
// still receives every key, so InputText, history and Tab completion behave
// normally, and we then consume the event before the guest pad driver at 0 can
// see it.
//
// `WantTextInput` is the correct predicate - it is true exactly while an ImGui
// text field has focus - and it is checked live, so closing the console
// restores guest input on the same event with no state of our own to get out of
// sync. The console's own toggle key is not ours to worry about: the SDK binds
// it through the same z-order-0 path we are shadowing, so it is checked before
// we would ever swallow it... except that it is not, which is why the toggle
// key is explicitly excluded below.

#pragma once

#include <imgui.h>

#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace thps::console_guard {

// Strictly between ImGuiDrawer (64) and MnkInputDriver / ReXApp keybinds (0).
inline constexpr size_t kZOrder = 32;

namespace detail {

class Guard final : public rex::ui::WindowInputListener {
 public:
  explicit Guard(rex::ui::ImGuiDrawer* drawer) : drawer_(drawer) {}

  void OnKeyDown(rex::ui::KeyEvent& e) override { Consume(e); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { Consume(e); }
  void OnKeyChar(rex::ui::KeyEvent& e) override { Consume(e); }

  uint64_t swallowed() const { return swallowed_; }

 private:
  void Consume(rex::ui::KeyEvent& e) {
    if (!drawer_ || e.is_handled()) return;
    // Live predicate: true exactly while an ImGui text field has focus.
    if (!drawer_->GetIO().WantTextInput) return;
    // Never swallow the console's own toggle, or it cannot be closed. The key
    // is read from the `bind_console` cvar rather than hardcoded, so a rebind
    // does not silently lock the console open.
    if (e.virtual_key() == ToggleKey()) return;
    e.set_handled(true);
    ++swallowed_;
  }

  static rex::ui::VirtualKey ToggleKey() {
    const std::string bind = rex::cvar::GetFlagByName("bind_console");
    if (!bind.empty()) {
      rex::ui::VirtualKey k = rex::ui::ParseVirtualKey(bind);
      if (k != rex::ui::VirtualKey::kNone) return k;
    }
    return rex::ui::VirtualKey::kOem3;  // '`~' on a US layout - the SDK default
  }

  rex::ui::ImGuiDrawer* drawer_ = nullptr;
  uint64_t swallowed_ = 0;
};

struct State {
  rex::ui::Window* window = nullptr;
  std::unique_ptr<Guard> guard;
};

inline State& state() {
  static State s;
  return s;
}

}  // namespace detail

inline void Attach(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer) {
  auto& s = detail::state();
  if (s.guard || !window || !drawer) return;
  s.window = window;
  s.guard = std::make_unique<detail::Guard>(drawer);
  window->AddInputListener(s.guard.get(), kZOrder);
  REXLOG_INFO("console-guard: attached at z-order {} (keeps console typing out "
              "of the guest pad)", kZOrder);
}

inline void Detach() {
  auto& s = detail::state();
  if (!s.guard) return;
  if (s.window) s.window->RemoveInputListener(s.guard.get());
  REXLOG_INFO("console-guard: detached after swallowing {} key events",
              s.guard->swallowed());
  s.guard.reset();
  s.window = nullptr;
}

inline uint64_t swallowed() {
  auto& s = detail::state();
  return s.guard ? s.guard->swallowed() : 0;
}

}  // namespace thps::console_guard
