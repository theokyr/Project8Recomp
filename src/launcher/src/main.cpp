// thps_p8_gui - the launcher front end.
//
// Build-up step 1-3 of the playbook's order: own the SDL loop from day one,
// require a zero-warning document load, and get a controller reaching every
// control before any decoration exists.
//
// The event loop is ours rather than RmlUi's Backend::ProcessEvents() because
// that function drains SDL_PollEvent inside its own translation unit and ends
// with `default: break;` - no SDL_EVENT_GAMEPAD_* can escape it. A launcher
// that cannot be driven from the couch is not the launcher we want, so we take
// RmlUi's platform and renderer interfaces and leave its loop.

#include <SDL3/SDL.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3_image/SDL_image.h>

#include <RmlUi/Core.h>

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_SDL.h"
#include "launcher_app.h"
#include "platform.h"

namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;

// Every RCSS parse failure and every RML structural complaint arrives through
// LogMessage. The old UI stack shipped 81 of these and nobody noticed, because
// a warning costs nothing at runtime and the screen still draws something. So
// they are counted here and the count is checked at startup rather than left as
// a log line somebody might scroll past.
class CountingSystemInterface final : public SystemInterface_SDL {
 public:
  explicit CountingSystemInterface(SDL_Window* window) : SystemInterface_SDL(window) {}

  bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
    switch (type) {
      case Rml::Log::LT_ERROR:
      case Rml::Log::LT_ASSERT:
        ++errors_;
        std::fprintf(stderr, "[rmlui:error] %s\n", message.c_str());
        break;
      case Rml::Log::LT_WARNING:
        ++warnings_;
        std::fprintf(stderr, "[rmlui:warn ] %s\n", message.c_str());
        break;
      default:
        break;
    }
    return true;
  }

  int warnings() const { return warnings_; }
  int errors() const { return errors_; }

 private:
  int warnings_ = 0;
  int errors_ = 0;
};

// Assets live beside the executable in an installed portable directory; the
// source tree is the fallback so the build tree runs without an install step.
std::filesystem::path ResolveAssetRoot() {
  if (const char* base = SDL_GetBasePath()) {
    std::filesystem::path beside = std::filesystem::path(base) / "assets";
    if (std::filesystem::exists(beside / "ui" / "home.rml")) {
      return beside;
    }
  }
  return std::filesystem::path(THPS_P8_GUI_SOURCE_ASSETS);
}

// A controller press becomes the same key event a keyboard would produce, so
// the navigation graph RCSS declares with `nav: auto` is the only navigation
// model in the program. Nothing downstream knows which device moved focus.
Rml::Input::KeyIdentifier GamepadButtonToKey(Uint8 button) {
  switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return Rml::Input::KI_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Rml::Input::KI_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Rml::Input::KI_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Rml::Input::KI_RIGHT;
    case SDL_GAMEPAD_BUTTON_SOUTH: return Rml::Input::KI_RETURN;
    case SDL_GAMEPAD_BUTTON_EAST: return Rml::Input::KI_ESCAPE;
    // LB/RB walk the tab ring. This is the safety net that makes a
    // hand-authored nav-* graph unnecessary: even if a spatial edge is missing,
    // every focusable element is still reachable.
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return Rml::Input::KI_TAB;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Rml::Input::KI_TAB;
    default: return Rml::Input::KI_UNKNOWN;
  }
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  // Resolved before the window exists, because the window icon comes out of it.
  const std::filesystem::path assets_for_icon = ResolveAssetRoot();

  SDL_Window* window = SDL_CreateWindow("Tony Hawk's Project 8", kDefaultWidth, kDefaultHeight,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  // A project-authored wordmark, never box art: the icon travels in the release
  // archive, and shipping any part of the game's own artwork would put guest
  // content in an artifact whose whole claim is that it carries none.
  //
  // A BMP, loaded by SDL itself rather than a PNG through SDL3_image. The PNG
  // version failed with "Unsupported image format" on a Windows build whose
  // package DID ship libpng beside it - SDL3_image resolves its codecs by
  // dlopen'ing them under names that vary by packaging, and the icon is not
  // worth depending on that. SDL_LoadBMP has no codec, no dynamic load and no
  // packaging question.
  //
  // Best-effort. A missing or unreadable icon is a cosmetic loss and must not
  // stop the launcher, which is why nothing here returns.
  {
    const std::filesystem::path icon_path = assets_for_icon / "ui" / "icon.bmp";
    if (SDL_Surface* icon = SDL_LoadBMP(icon_path.string().c_str())) {
      SDL_SetWindowIcon(window, icon);
      SDL_DestroySurface(icon);
    } else {
      std::fprintf(stderr, "thps_p8_gui: no window icon at %s (%s)\n",
                   icon_path.string().c_str(), SDL_GetError());
    }
  }

  SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer) {
    std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  CountingSystemInterface system_interface(window);
  RenderInterface_SDL render_interface(renderer);
  Rml::SetSystemInterface(&system_interface);
  Rml::SetRenderInterface(&render_interface);

  if (!Rml::Initialise()) {
    std::fprintf(stderr, "Rml::Initialise failed\n");
    return 1;
  }

  const std::filesystem::path& assets = assets_for_icon;
  const std::filesystem::path ui = assets / "ui";

  // Loaded before any document: a document that references a missing family
  // silently falls back and the layout is wrong in ways that read as bad RCSS.
  const struct { const char* file; bool fallback; } faces[] = {
      {"fonts/NotoSans-Regular.ttf", true},
      {"fonts/NotoSans-SemiBold.ttf", false},
      {"fonts/NotoSans-Bold.ttf", false},
  };
  for (const auto& face : faces) {
    const std::string path = (ui / face.file).string();
    if (!Rml::LoadFontFace(path, face.fallback)) {
      std::fprintf(stderr, "failed to load font face: %s\n", path.c_str());
      return 1;
    }
  }

  int pixel_w = kDefaultWidth;
  int pixel_h = kDefaultHeight;
  SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);

  Rml::Context* context = Rml::CreateContext("launcher", Rml::Vector2i(pixel_w, pixel_h));
  if (!context) {
    std::fprintf(stderr, "Rml::CreateContext failed\n");
    return 1;
  }
  context->SetDensityIndependentPixelRatio(SDL_GetWindowDisplayScale(window));

  // The portable directory is wherever the executable lives: the install is a
  // folder, not a set of XDG paths.
  const std::filesystem::path portable_dir = thps::platform::SelfDir();

  thps::LauncherApp app(context, ui, portable_dir);
  if (!app.Initialise()) {
    std::fprintf(stderr, "failed to load launcher documents from %s\n", ui.string().c_str());
    return 1;
  }

  // The gate the old stack never had. Anything that failed to parse has already
  // reported itself by this point, so this is the earliest place the answer is
  // known and the loudest place to say it.
  std::printf("thps_p8_gui: RmlUi %s, document loaded with %d warning(s), %d error(s)\n",
              THPS_P8_GUI_RMLUI_COMMIT, system_interface.warnings(), system_interface.errors());
  if (const char* strict = SDL_getenv("THPS_P8_GUI_STRICT")) {
    if (strict[0] == '1' && (system_interface.warnings() || system_interface.errors())) {
      std::fprintf(stderr, "thps_p8_gui: refusing to run with RCSS/RML diagnostics (STRICT)\n");
      return 2;
    }
  }
  if (SDL_getenv("THPS_P8_GUI_EXIT_AFTER_LOAD")) {
    Rml::Shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return (system_interface.warnings() || system_interface.errors()) ? 2 : 0;
  }

  // Test hook. Synthetic clicks are unreliable under a bare X server with no
  // window manager, so the handoff path needs a way to be exercised that does
  // not depend on one. Drives the real action, not a shortcut around it.
  if (const char* action = SDL_getenv("THPS_P8_GUI_TEST_ACTION")) {
    app.OnAction(action);
  }

  std::unordered_map<SDL_JoystickID, SDL_Gamepad*> pads;
  int pad_count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&pad_count)) {
    for (int i = 0; i < pad_count; ++i) {
      if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
        pads[ids[i]] = pad;
      }
    }
    SDL_free(ids);
  }
  std::printf("thps_p8_gui: %d gamepad(s) connected at start\n", pad_count);

  bool running = true;
  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {
        case SDL_EVENT_QUIT:
          running = false;
          break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
          context->SetDimensions(Rml::Vector2i(ev.window.data1, ev.window.data2));
          context->SetDensityIndependentPixelRatio(SDL_GetWindowDisplayScale(window));
          break;

        // Hotplug: a pad connected after launch must work without a restart,
        // because the common case is picking the controller up after the
        // window is already open.
        case SDL_EVENT_GAMEPAD_ADDED:
          if (SDL_Gamepad* pad = SDL_OpenGamepad(ev.gdevice.which)) {
            pads[ev.gdevice.which] = pad;
            std::printf("thps_p8_gui: gamepad connected: %s\n", SDL_GetGamepadName(pad));
          }
          break;

        case SDL_EVENT_GAMEPAD_REMOVED:
          if (auto it = pads.find(ev.gdevice.which); it != pads.end()) {
            SDL_CloseGamepad(it->second);
            pads.erase(it);
            std::printf("thps_p8_gui: gamepad disconnected\n");
          }
          break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
          // East is Back everywhere in this UI, and RmlUi has no default action
          // for KI_ESCAPE, so it would fall into the void if left to the
          // document. Handled here rather than mapped to a key.
          if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
            app.OnAction("back");
            break;
          }
          const Rml::Input::KeyIdentifier key = GamepadButtonToKey(ev.gbutton.button);
          if (key != Rml::Input::KI_UNKNOWN) {
            const int modifier =
                (ev.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? Rml::Input::KM_SHIFT : 0;
            context->ProcessKeyDown(key, modifier);
          }
          break;
        }

        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
          const Rml::Input::KeyIdentifier key = GamepadButtonToKey(ev.gbutton.button);
          if (key != Rml::Input::KI_UNKNOWN) {
            context->ProcessKeyUp(key, 0);
          }
          break;
        }

        case SDL_EVENT_KEY_DOWN:
          if (ev.key.key == SDLK_ESCAPE) {
            running = false;
            break;
          }
          [[fallthrough]];

        default:
          RmlSDL::InputEventHandler(context, window, ev);
          break;
      }
    }

    // Drains the file-dialog handoff, which SDL may have completed on another
    // thread. Nothing outside this call touches RmlUi off the main thread.
    app.Poll();
    if (app.quit_requested()) running = false;

    context->Update();

    SDL_SetRenderDrawColor(renderer, 12, 18, 28, 255);
    SDL_RenderClear(renderer);
    render_interface.BeginFrame();
    context->Render();
    render_interface.EndFrame();
    SDL_RenderPresent(renderer);
  }

  for (auto& [id, pad] : pads) {
    (void)id;
    SDL_CloseGamepad(pad);
  }
  const std::vector<std::string> launch = app.LaunchArgv();

  Rml::Shutdown();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  if (!launch.empty()) {
    // Replace this process rather than becoming its parent. The GUI is one-shot
    // and never returns, so there is nothing for it to wait for, and on POSIX
    // replacing the image means no orphaned parent is left holding the window's
    // GPU file descriptors while the game runs. Windows cannot replace an
    // image and spawns instead - platform_windows.inc says why that still
    // answers the same concern.
    //
    // After Rml::Shutdown and SDL_Quit on purpose: the handoff must not inherit
    // a live renderer.
    std::printf("thps_p8_gui: handing off to %s\n", launch[0].c_str());
    std::fflush(stdout);
    thps::platform::ExecReplace(launch);  // does not return
  }
  return 0;
}
