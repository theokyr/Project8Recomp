#include "launcher_app.h"

#include "common/supported_dumps.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/StringUtilities.h>

namespace thps {
namespace {

const char* StateClass(State s) {
  switch (s) {
    case State::kWelcome: return "st-welcome";
    case State::kPicking: return "st-picking";
    case State::kVerifying: return "st-verifying";
    case State::kExtracting: return "st-extracting";
    case State::kDone: return "st-done";
    case State::kFailNotDisc: return "st-fail";
    case State::kFailWrongBuild: return "st-fail";
    case State::kFailNotGame: return "st-fail";
    case State::kFailIo: return "st-fail";
    case State::kFailNoSpace: return "st-fail";
    // Not a failure: the user asked for it. It offers the same "choose another
    // file" way forward, so it shares the panel and not the tone.
    case State::kCancelled: return "st-fail";
    case State::kReady: return "st-ready";
    case State::kStarting: return "st-starting";
    case State::kDebris: return "st-debris";
    case State::kBlocked: return "st-blocked";
    case State::kSettings: return "st-settings";
    case State::kAbout: return "st-about";
  }
  return "st-welcome";
}

// Every class the gating stylesheet knows about, so switching states can clear
// the previous one without tracking which it was.
constexpr const char* kAllStateClasses[] = {
    "st-welcome", "st-picking",  "st-verifying", "st-extracting", "st-done",
    "st-fail",    "st-ready",    "st-starting",  "st-debris",     "st-blocked",
    "st-settings", "st-about",
};

// Failure copy. No developer vocabulary reaches a player. There is no
// "sha256", no "XEX", no "title id" here - the first sentence says what is
// wrong in disc terms, the second says what to do about it.
struct Copy {
  const char* heading;
  const char* detail;
};

Copy CopyFor(State s) {
  switch (s) {
    case State::kWelcome:
      return {"Set up your game",
              "Choose the disc image made from your own Tony Hawk's Project 8 copy. "
              "The image stays where it is. Nothing is moved, changed, or uploaded."};
    case State::kPicking:
      return {"Choose your disc image", "Pick the file you made from your own disc."};
    case State::kVerifying:
      return {"Checking your disc", "This takes a moment."};
    case State::kExtracting:
      return {"Copying your game", "This happens once. After it finishes you will not need the "
                                   "disc image again on this machine."};
    case State::kDone:
      return {"Ready to play", "Setup is complete. Future launches go straight to Play."};
    case State::kFailNotDisc:
      return {"That file is not a game disc",
              "It does not look like a disc image. Choose the file you made from your "
              "Tony Hawk's Project 8 disc, usually an .iso file."};
    case State::kFailWrongBuild:
      return {"That is a different release",
              "This is Tony Hawk's Project 8, but a different release of it than this port was "
              "built from. Only the supported 2006 retail disc works with this build."};
    case State::kFailNotGame:
      return {"That is a different game",
              "The disc image opened, but it is not Tony Hawk's Project 8."};
    case State::kFailIo:
      return {"Could not read that file",
              "The file could not be opened, or the game could not be copied. "
              "Check the file is complete and try again."};
    case State::kFailNoSpace:
      // Deliberately said before anything is copied. The previous behaviour
      // wrote until the disk filled and then reported "there was not enough
      // room" - the right sentence, hours late and with a partial install left
      // behind.
      return {"Not enough room",
              "There is not enough free space on this drive to copy the game. Free some "
              "space, or move this folder to a drive with more room, and try again."};
    case State::kCancelled:
      return {"Setup stopped",
              "The copy was stopped and the partly-copied files have been removed. "
              "No partial game files remain."};
    case State::kReady:
      return {"Ready to play", "Your game is set up on this machine."};
    case State::kStarting:
      return {"Starting", "Opening the game."};
    case State::kDebris:
      // "orphaned shared-memory segment" is exactly the vocabulary to avoid.
      // What a player needs to know is that a past crash left memory behind and
      // one button gets it back.
      return {"Some memory needs freeing",
              "A previous session stopped unexpectedly and left temporary game memory in use. "
              "Free it before starting again."};
    case State::kBlocked:
      return {"The game is already running",
              "Close the running copy first, or stop it from here."};
    case State::kSettings:
      return {"Settings", "Changes apply the next time you press Play."};
    case State::kAbout:
      return {"About", "About this port and its third-party software."};
  }
  return {"", ""};
}

// Buttons announce themselves by id; one listener serves every document.
class ActionListener final : public Rml::EventListener {
 public:
  explicit ActionListener(LauncherApp* app) : app_(app) {}
  void ProcessEvent(Rml::Event& event) override {
    // GetTargetElement, not GetCurrentElement. This listener is attached to the
    // document so that panels appearing later are covered without rebinding,
    // and GetCurrentElement returns the element the listener is *attached to* -
    // the document, whose id is empty. Every click was therefore discarded,
    // from the pad and from the mouse alike.
    Rml::Element* element = event.GetTargetElement();
    // A click can land on a child of the button (its text), so walk up until
    // something names itself. Stops at the document, which has no id.
    while (element) {
      const Rml::String id = element->GetId();
      if (!id.empty()) {
        app_->OnAction(id);
        return;
      }
      element = element->GetParentNode();
    }
  }

 private:
  LauncherApp* app_;
};

ActionListener* g_listener = nullptr;

void AttachActions(Rml::ElementDocument* doc, LauncherApp* app) {
  if (!g_listener) g_listener = new ActionListener(app);
  // Document-level, so panels that appear later are covered without rebinding.
  doc->AddEventListener(Rml::EventId::Click, g_listener);
}

// Sizes in the units a player thinks in. GB, not GiB: this number sits next to
// what a file manager says about the same drive, and disagreeing with it by 7%
// would read as a bug in the launcher.
std::string HumanBytes(std::uint64_t bytes) {
  char buf[64];
  const double gb = double(bytes) / 1e9;
  if (gb >= 1.0) {
    std::snprintf(buf, sizeof(buf), "%.1f GB", gb);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f MB", double(bytes) / 1e6);
  }
  return buf;
}

// Extraction writes the disc's own byte count, plus room for the filesystem's
// own overhead and for the install not to land a user at zero free space.
// Deliberately generous: refusing a copy that would have fitted costs the user
// a settings change, and allowing one that does not costs them the whole copy.
constexpr std::uint64_t kSpaceHeadroomBytes = 512ull * 1000 * 1000;

}  // namespace

void PendingPick::Set(std::string path, bool cancelled) {
  std::lock_guard<std::mutex> lock(mutex_);
  has_ = true;
  cancelled_ = cancelled;
  path_ = std::move(path);
}

bool PendingPick::Take(std::string* out_path, bool* out_cancelled) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!has_) return false;
  has_ = false;
  *out_path = path_;
  *out_cancelled = cancelled_;
  return true;
}

LauncherApp::LauncherApp(Rml::Context* context, std::filesystem::path ui_dir,
                         std::filesystem::path portable_dir)
    : context_(context), ui_dir_(std::move(ui_dir)), portable_dir_(std::move(portable_dir)) {}

LauncherApp::~LauncherApp() {
  // Closing the window mid-extract used to leave the worker running and the
  // partial bytes on disk. Stop it and join, so the process does not outlive
  // the UI that was reporting on it.
  CancelExtract();
  JoinExtractThread();
}

bool LauncherApp::IsInstalled() const {
  std::error_code ec;
  return std::filesystem::exists(portable_dir_ / "config" / "install.toml", ec) &&
         std::filesystem::exists(portable_dir_ / "game" / "default.xex", ec);
}

std::filesystem::path LauncherApp::SettingsFile() const {
  return portable_dir_ / "config" / "settings.toml";
}

bool LauncherApp::Initialise() {
  first_run_ = context_->LoadDocument((ui_dir_ / "first_run.rml").string());
  home_ = context_->LoadDocument((ui_dir_ / "home.rml").string());
  if (!first_run_ || !home_) return false;

  AttachActions(first_run_, this);
  AttachActions(home_, this);

  const bool had_settings = LoadSettings(SettingsFile(), &settings_);
  EnumerateDisplays();
  if (!had_settings) SeedHandheldDefaults();

  // Before deciding which screen to open on: an install can be complete and
  // still be missing its marker.
  AdoptExistingInstall();

  if (IsInstalled()) {
    ReadLastRun();
    ShowScreen(Screen::kHome);
    RefreshHomeState();
  } else {
    ScanForNearbyDisc();
    ShowScreen(Screen::kFirstRun);
    SetState(State::kWelcome);
    ShowNearbyDisc();
  }
  return true;
}

// An already-extracted game directory with no install marker.
//
// WHY THIS EXISTS
//
// IsInstalled() wants two things: `game/default.xex` and `config/install.toml`.
// The marker is written last by the extraction step, deliberately, so that a
// death mid-extract leaves a first-run install rather than a broken one.
//
// That is right for an extraction this launcher performed, and wrong for an
// install that arrived any other way. A portable directory copied from another
// machine, or deployed to a Steam Deck with the disc tree rsync'd in beside it,
// has a complete and valid game and no marker - and the launcher responds by
// asking a handheld with no keyboard to browse for a disc image it already has.
// "Copy the folder to another device and play" is the install model this
// project chose, so the launcher has to recognise the result of it.
//
// The size check is a sanity signal and explicitly NOT an identity check -
// `supported_dumps.h` says xex_size "sharpens the 'truncated' message only".
// Identity stays where it already is: the game's own OnConfigurePaths gate
// hashes default.xex against the same table and exits 3 on a mismatch, and that
// gate guards every way in. All this decides is which screen to open on, so the
// cost of being wrong is a Play button that then refuses with a clear message,
// not a bad launch.
void LauncherApp::AdoptExistingInstall() {
  std::error_code ec;
  if (std::filesystem::exists(portable_dir_ / "config" / "install.toml", ec)) return;

  const std::filesystem::path xex = portable_dir_ / "game" / "default.xex";
  if (!std::filesystem::exists(xex, ec)) return;
  const std::uintmax_t size = std::filesystem::file_size(xex, ec);
  if (ec) return;

  bool plausible = false;
  for (const auto& dump : thps::identify::kSupportedDumps) {
    if (size == dump.xex_size) {
      plausible = true;
      break;
    }
  }
  if (!plausible) return;

  std::filesystem::create_directories(portable_dir_ / "config", ec);
  const std::filesystem::path file = portable_dir_ / "config" / "install.toml";
  const std::filesystem::path temp = file.string() + ".new";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    // `source_image` is provenance only and nothing reads it back; "adopted"
    // records that no image was picked on this machine.
    out << "[install]\ngame_data = \"game\"\nsource_image = \"adopted\"\n";
    if (!out) return;
  }
  std::filesystem::rename(temp, file, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return;
  }
  std::fprintf(stderr, "thps_p8_gui: adopted an existing game directory at %s\n",
               (portable_dir_ / "game").string().c_str());
}

// A disc image sitting next to the launcher.
//
// WHY THIS EXISTS
//
// The install is one portable directory, and the intended shape is that a
// player drops their own dump beside it and runs the launcher. Until now the
// only way to say which file that was, was SDL_ShowOpenFileDialog - a file
// browser, driven by a mouse. On a Steam Deck there is no mouse and no
// keyboard, and a directory tree navigated with a trackpad is the worst
// possible first thirty seconds of a port that otherwise plays with a pad.
//
// Scanning is cheap if it is done in the right order. Identity costs about
// 58 ms per candidate - fine once, not fine across a Downloads folder - so the
// size filter runs first and rejects everything that could not be this disc
// before any subprocess is spawned. The retail image is 7.3 GB; a 6 GB floor
// leaves room for a differently-packed dump without admitting a stray ISO.
//
// Deliberately NOT recursive, and deliberately only two directories. A scan
// that wanders is a scan that hashes somebody's entire media library on
// startup, and "why did the launcher take four minutes to open" is a much
// worse bug than "I had to press Browse".
void LauncherApp::ScanForNearbyDisc() {
  // 6 GB. Below this it cannot be an XGD2 image of this title, whatever it is.
  constexpr std::uintmax_t kMinDiscBytes = 6ull * 1000 * 1000 * 1000;

  const std::filesystem::path tool =
      platform::SiblingExe(portable_dir_, "thps_p8_identify");
  std::error_code ec;
  if (!std::filesystem::exists(tool, ec)) return;

  std::vector<std::pair<std::uintmax_t, std::filesystem::path>> candidates;
  for (const auto& dir : {portable_dir_, portable_dir_ / "disc"}) {
    if (!std::filesystem::is_directory(dir, ec)) continue;
    for (const auto& entry : std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (ec) break;
      if (!entry.is_regular_file(ec)) continue;
      std::string ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (ext != ".iso" && ext != ".img") continue;
      const std::uintmax_t size = entry.file_size(ec);
      if (ec || size < kMinDiscBytes) continue;
      candidates.emplace_back(size, entry.path());
    }
  }
  // Largest first: if a directory holds both a full dump and something else
  // over the floor, the full dump is the better first guess.
  std::sort(candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  // Bounded regardless of what is in the directory. Four probes is a quarter
  // of a second in the worst case.
  constexpr size_t kMaxProbes = 4;
  size_t probed = 0;
  for (const auto& [size, path] : candidates) {
    if (probed++ >= kMaxProbes) break;
    const platform::RunResult result =
        platform::Run({tool.string(), "--identify_disc=" + path.string(), "--json"});
    if (result.ran && result.exit_code == 0 &&
        result.out.find("\"verdict\": \"EXACT\"") != std::string::npos) {
      nearby_disc_ = path.string();
      std::fprintf(stderr, "thps_p8_gui: found a supported disc image beside "
                           "the launcher: %s\n", nearby_disc_.c_str());
      return;
    }
  }
}

// Revealed with an inline style rather than a class, because the panel these
// live in is itself shown by a `display: flex` rule keyed on the state class;
// a competing class rule would be a cascade argument rather than a decision.
void LauncherApp::ShowNearbyDisc() {
  if (!active_ || nearby_disc_.empty()) return;
  const std::filesystem::path path(nearby_disc_);
  SetText("nearby_text", "Found " + path.filename().string() + " next to the "
                         "launcher.");
  if (Rml::Element* note = active_->GetElementById("nearby")) {
    note->SetProperty("display", "block");
  }
  if (Rml::Element* button = active_->GetElementById("install_nearby")) {
    button->SetProperty("display", "inline-block");
    // The pad lands here rather than on Browse: it is the action the player
    // almost certainly wants, and on a handheld it is also the only one of the
    // two that can be completed without a mouse.
    button->Focus();
  }
}

// First-run defaults for a handheld panel.
//
// The game's own default is a 720p borderless window on the primary display,
// which is right on a desktop and wrong on a Steam Deck: a 1280x720 window on
// a 1280x800 panel is neither fullscreen nor comfortably windowed, and there
// is no window manager to fix it with.
//
// 720p fullscreen is the arm chosen here, so the guest renders a video mode
// the 2006 title actually shipped and the compositor letterboxes it. The 16:10
// alternative is offered in Settings; which one becomes the default is a
// question about how the HUD and the FMVs are framed, and that is decided from
// screenshots taken on the hardware rather than from here.
//
// Only ever on first run. A settings file that exists is the player's, and a
// launcher that re-decides display settings behind somebody's back is worse
// than one that guessed wrong once.
void LauncherApp::SeedHandheldDefaults() {
  if (displays_.size() < 2) return;  // index 0 is the "Game default" entry

  SDL_DisplayID* ids = nullptr;
  int count = 0;
  ids = SDL_GetDisplays(&count);
  if (!ids || count < 1) {
    if (ids) SDL_free(ids);
    return;
  }
  SDL_Rect bounds{};
  const bool have = SDL_GetDisplayBounds(ids[0], &bounds);
  SDL_free(ids);
  if (!have) return;

  // The Deck's panel exactly. Deliberately not a "small screen" heuristic:
  // seeding settings is a decision, and it should fire on the hardware it was
  // reasoned about rather than on anything that happens to be short.
  if (bounds.w != 1280 || bounds.h != 800) return;

  settings_.resolution = "720p";
  settings_.fullscreen = 1;
  SaveSettings(SettingsFile(), settings_);
  std::fprintf(stderr, "thps_p8_gui: 1280x800 panel detected; defaulting to "
                       "720p fullscreen\n");
}

void LauncherApp::EnumerateDisplays() {
  displays_.clear();
  // "Game default" first, so the list always has a way back to not choosing.
  // Its index is -1, which RenderArgv reads as "emit no --monitor at all".
  displays_.push_back({-1, "Game default"});

  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  if (!ids) return;
  for (int i = 0; i < count; ++i) {
    const char* name = SDL_GetDisplayName(ids[i]);
    std::string label = name ? name : "Display";
    // The refresh rate is the part that matters here and the part a display's
    // name never carries: the port's frame pacing follows the guest refresh, so
    // "which of my two monitors" is really "60 Hz or 144 Hz".
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(ids[i])) {
      char detail[96];
      std::snprintf(detail, sizeof(detail), " (%dx%d @ %d Hz)", mode->w, mode->h,
                    int(mode->refresh_rate + 0.5f));
      label += detail;
    }
    // The game's --monitor counts in SDL's display ordinal, which is the index
    // in this array and NOT the SDL_DisplayID.
    displays_.push_back({i, label});
  }
  SDL_free(ids);
}

void LauncherApp::ShowScreen(Screen screen) {
  screen_ = screen;
  Rml::ElementDocument* next = (screen == Screen::kHome) ? home_ : first_run_;
  if (active_ == next) return;
  if (active_) active_->Hide();
  active_ = next;
  active_->Show();
}

void LauncherApp::ApplyStateClass() {
  if (!active_) return;
  // The document element itself carries the class; <body> is the document.
  Rml::Element* target = active_;
  for (const char* c : kAllStateClasses) target->SetClass(c, false);
  target->SetClass(StateClass(state_), true);
}

void LauncherApp::SetText(const char* element_id, const std::string& text) {
  if (!active_) return;
  if (Rml::Element* e = active_->GetElementById(element_id)) {
    // Paths and display names are untrusted text. SetInnerRML parses markup,
    // so escape every value before inserting it; a filename containing '<' or
    // '&' must remain visible text rather than becoming a broken RML element.
    e->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
  }
}

void LauncherApp::SetState(State state) {
  state_ = state;
  ApplyStateClass();
  const Copy copy = CopyFor(state);
  SetText("heading", copy.heading);
  SetText("detail", copy.detail);
  // The pill is a sentence, not a dot, and it is the status on the home screen
  // (there is deliberately no duplicate <h2> there), so it moves with the state.
  switch (state) {
    case State::kReady: SetText("status_pill", "Ready to play"); break;
    case State::kDebris: SetText("status_pill", "Needs a moment"); break;
    case State::kBlocked: SetText("status_pill", "Already running"); break;
    case State::kStarting: SetText("status_pill", "Starting"); break;
    case State::kSettings: SetText("status_pill", "Settings"); break;
    case State::kAbout: SetText("status_pill", "About"); break;
    default: break;
  }
  // Green means "ready to play" and nothing else. The sub-screens reuse the
  // pill as a heading, so they must not inherit the colour that says the
  // install is fine - it is the one element on screen a player reads as status
  // rather than as a label.
  if (active_) {
    if (Rml::Element* pill = active_->GetElementById("status_pill")) {
      pill->SetClass("ready", state == State::kReady);
    }
  }
  if (!disc_path_.empty()) SetText("disc_path", disc_path_);
  if (state == State::kSettings) RenderSettingsPanel();

  // Focus has to be restored explicitly after a structural change, or the pad
  // is left pointing at an element that is no longer displayed and the next
  // press goes nowhere. Each state names the control it wants to land on.
  if (active_) {
    const char* wanted = nullptr;
    switch (state) {
      case State::kWelcome: wanted = "browse"; break;
      case State::kDone: wanted = "continue_home"; break;
      case State::kFailNotDisc:
      case State::kFailWrongBuild:
      case State::kFailNotGame:
      case State::kFailIo:
      case State::kFailNoSpace:
      case State::kCancelled: wanted = "retry"; break;
      case State::kReady: wanted = "play"; break;
      case State::kDebris: wanted = "reclaim"; break;
      case State::kBlocked: wanted = "stop"; break;
      case State::kSettings: wanted = "set_resolution"; break;
      case State::kAbout: wanted = "about_back"; break;
      // Extracting owns one: Cancel is the only control on that screen, and a
      // screen with a live control and no focus cannot be driven by a pad.
      case State::kExtracting: wanted = "cancel_extract"; break;
      default: break;  // transient states own no control
    }
    if (wanted) {
      if (Rml::Element* focus = active_->GetElementById(wanted)) focus->Focus();
    }
  }
}

void LauncherApp::BeginPick() {
  SetState(State::kPicking);
  static const SDL_DialogFileFilter filters[] = {
      {"Disc image", "iso;img"},
      {"All files", "*"},
  };
  SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* filelist, int /*filter*/) {
        auto* self = static_cast<LauncherApp*>(userdata);
        if (!filelist || !filelist[0]) {
          self->pending_pick_.Set({}, /*cancelled=*/true);
          return;
        }
        self->pending_pick_.Set(filelist[0], /*cancelled=*/false);
      },
      this, nullptr, filters, 2, nullptr, false);
}

bool LauncherApp::CheckSpaceFor(std::uint64_t needed) {
  const std::uint64_t available = platform::FreeSpaceBytes(portable_dir_);
  // 0 means the query failed, not that the disk is full. Refusing here would
  // turn "we could not ask" into "you cannot install", which is a worse answer
  // than letting the copy try and report honestly if it runs out.
  if (available == 0) {
    std::fprintf(stderr, "thps_p8_gui: free space unknown for %s; proceeding\n",
                 portable_dir_.string().c_str());
    return true;
  }
  if (available >= needed + kSpaceHeadroomBytes) return true;

  std::fprintf(stderr, "thps_p8_gui: need %llu bytes + headroom, have %llu\n",
               (unsigned long long)needed, (unsigned long long)available);
  SetState(State::kFailNoSpace);
  // The numbers belong on screen, not only in a log nobody will read: "not
  // enough room" is unactionable without knowing how much is missing.
  char text[320];
  std::snprintf(text, sizeof(text),
                "The game needs about %s and this drive has %s free. Free some space, or "
                "move this folder to a drive with more room, and try again.",
                HumanBytes(needed + kSpaceHeadroomBytes).c_str(), HumanBytes(available).c_str());
  SetText("detail", text);
  return false;
}

void LauncherApp::VerifyPicked(const std::string& path) {
  disc_path_ = path;
  disc_bytes_ = 0;
  SetState(State::kVerifying);
  // Identity is answered by the SDK-side worker, never here: this process links
  // no SDK and must not learn to read a disc image.
  const std::filesystem::path tool = platform::SiblingExe(portable_dir_, "thps_p8_identify");
  std::error_code ec;
  if (!std::filesystem::exists(tool, ec)) {
    std::fprintf(stderr, "thps_p8_gui: no thps_p8_identify beside the launcher\n");
    SetState(State::kFailIo);
    return;
  }
  // Synchronous on purpose: the measured cost of mount + resolve + hash is
  // about 58 ms, which is inside one frame at 60 Hz. A thread here would buy
  // nothing and cost a marshalling layer.
  const platform::RunResult result =
      platform::Run({tool.string(), "--identify_disc=" + path, "--json"});
  if (result.ran && result.exit_code == 0 &&
      result.out.find("\"verdict\": \"EXACT\"") != std::string::npos) {
    // The identity JSON already carries the disc's total byte count, so the
    // free-space question is answered from a measurement rather than a guess -
    // and answered here, between "this is the right disc" and the first byte
    // written.
    const size_t at = result.out.find("\"bytes\"");
    if (at != std::string::npos) {
      const size_t colon = result.out.find(':', at);
      if (colon != std::string::npos) {
        disc_bytes_ = std::strtoull(result.out.c_str() + colon + 1, nullptr, 10);
      }
    }
    if (disc_bytes_ > 0 && !CheckSpaceFor(disc_bytes_)) return;
    BeginExtract();
    return;
  }
  if (result.out.find("SAME_TITLE_WRONG_BUILD") != std::string::npos) {
    SetState(State::kFailWrongBuild);
    return;
  }
  if (result.out.find("NOT_THIS_GAME") != std::string::npos) {
    SetState(State::kFailNotGame);
    return;
  }
  if (result.out.find("NOT_A_DISC") != std::string::npos) {
    SetState(State::kFailNotDisc);
    return;
  }
  SetState(State::kFailIo);
}

void LauncherApp::Poll() {
  std::string path;
  bool cancelled = false;
  if (pending_pick_.Take(&path, &cancelled)) {
    if (cancelled) {
      SetState(State::kWelcome);
    } else {
      VerifyPicked(path);
    }
  }

  if (state_ == State::kExtracting) {
    const int result = extract_result_.load();
    if (result == 0) {
      const double fraction = extract_progress_.load();
      // A real bar, not a percentage inside a sentence. The worker has always
      // emitted `PROGRESS <fraction> <done> <total>` per chunk, so everything
      // needed was already on the wire.
      if (active_) {
        if (Rml::Element* bar = active_->GetElementById("extract_bar")) {
          bar->SetAttribute("value", float(fraction));
        }
      }
      char text[160];
      if (extract_total_.load() > 0) {
        std::snprintf(text, sizeof(text), "%d%% (%s of %s)", int(fraction * 100.0),
                      HumanBytes(extract_done_.load()).c_str(),
                      HumanBytes(extract_total_.load()).c_str());
      } else {
        std::snprintf(text, sizeof(text), "%d%%", int(fraction * 100.0));
      }
      SetText("extract_progress_text", text);
    } else {
      JoinExtractThread();
      if (result == 1) {
        FinishInstall();
      } else if (result == 3) {
        SetState(State::kCancelled);
      } else {
        SetState(State::kFailIo);
      }
    }
  }
}

void LauncherApp::OnAction(const Rml::String& id) {
  if (HandleSettingsAction(id)) return;

  if (id == "install_nearby") {
    // Straight to VerifyPicked, which is the same entry point the file dialog
    // uses - the scan already proved this file is EXACT, but identity is cheap
    // and going through one path means the free-space gate, the failure states
    // and the extraction cannot differ between the two ways in.
    if (!nearby_disc_.empty()) VerifyPicked(nearby_disc_);
  } else if (id == "browse" || id == "retry") {
    BeginPick();
  } else if (id == "continue_home") {
    ShowScreen(Screen::kHome);
    RefreshHomeState();
  } else if (id == "folder") {
    OpenPortableFolder("saves");
  } else if (id == "logs") {
    OpenPortableFolder("logs");
  } else if (id == "cancel_extract") {
    CancelExtract();
  } else if (id == "reclaim") {
    const std::filesystem::path launcher = platform::SiblingExe(portable_dir_, "thps_p8_launch");
    (void)platform::Run({launcher.string(), "--sweep-only"});
    RefreshHomeState();
  } else if (id == "stop") {
    // The pids come from --check --json, so the GUI never goes looking for
    // processes itself.
    for (const int pid : last_check_.game_pids) {
      platform::RequestStopByPid(pid);
    }
    RefreshHomeState();
  } else if (id == "settings") {
    ShowSettings();
  } else if (id == "about") {
    return_state_ = State::kReady;
    SetState(State::kAbout);
  } else if (id == "play") {
    SetState(State::kStarting);
    // The GUI does not come back after the game quits, so it does not wait for
    // it either: main() replaces this process with the launcher on the way out.
    // That keeps process lifetime and shm cleanup entirely in thps_p8_launch,
    // which is the one binary that survives a SIGKILL of the game.
    should_launch_ = true;
    quit_requested_ = true;
  } else if (id == "back" || id == "settings_back" || id == "about_back") {
    // Back out of a sub-screen if we are on one; otherwise Back means quit.
    // East on the pad reaches this too, which is why it cannot simply quit.
    if (state_ == State::kSettings || state_ == State::kAbout) {
      RefreshHomeState();
    } else if (state_ == State::kExtracting) {
      CancelExtract();
    } else {
      quit_requested_ = true;
    }
  } else if (id == "quit" || id == "close") {
    quit_requested_ = true;
  }
}

// --------------------------------------------------------------- settings ---

void LauncherApp::ShowSettings() {
  return_state_ = State::kReady;
  EnumerateDisplays();  // a monitor may have been plugged in since startup
  SetState(State::kSettings);
}

void LauncherApp::RenderSettingsPanel() {
  if (!active_) return;

  // Each control is a button that cycles its own choices. Four settings and a
  // pad to drive them: a cycling button is reachable with one bound key and
  // needs no popup, no list widget and no focus trap, where a dropdown would
  // need all three.
  const char* res_label = settings_.resolution.empty() ? "Game default"
                                                       : settings_.resolution.c_str();
  SetText("set_resolution_value", res_label);

  std::string monitor_label = "Game default";
  for (const DisplayOption& display : displays_) {
    if (display.index == settings_.monitor) {
      monitor_label = display.name;
      break;
    }
  }
  SetText("set_monitor_value", monitor_label);

  SetText("set_fullscreen_value", settings_.fullscreen == -1  ? "Game default"
                                  : settings_.fullscreen == 1 ? "Fullscreen"
                                                              : "Windowed");
  SetText("set_vsync_value", settings_.vsync == -1  ? "Game default"
                             : settings_.vsync == 1 ? "On"
                                                    : "Off");

  // Unset reads as On, because that is what RenderArgv does with it.
  SetText("set_performance_value", settings_.performance == 0 ? "Off" : "On");
}

bool LauncherApp::HandleSettingsAction(const Rml::String& id) {
  if (state_ != State::kSettings) return false;

  // Tri-states cycle unset -> on -> off -> unset, so "leave it to the game" is
  // always reachable again without a reset button.
  auto cycle_tri = [](int value) { return value == -1 ? 1 : (value == 1 ? 0 : -1); };

  if (id == "set_resolution" || id == "set_resolution_value") {
    const auto& choices = Settings::ResolutionChoices();
    size_t at = 0;
    for (size_t i = 0; i < choices.size(); ++i) {
      if (choices[i] == settings_.resolution) at = i;
    }
    settings_.resolution = choices[(at + 1) % choices.size()];
  } else if (id == "set_monitor" || id == "set_monitor_value") {
    size_t at = 0;
    for (size_t i = 0; i < displays_.size(); ++i) {
      if (displays_[i].index == settings_.monitor) at = i;
    }
    settings_.monitor = displays_.empty() ? -1 : displays_[(at + 1) % displays_.size()].index;
  } else if (id == "set_fullscreen" || id == "set_fullscreen_value") {
    settings_.fullscreen = cycle_tri(settings_.fullscreen);
  } else if (id == "set_vsync" || id == "set_vsync_value") {
    settings_.vsync = cycle_tri(settings_.vsync);
  } else if (id == "set_performance" || id == "set_performance_value") {
    // Two states, not three. The other rows have a meaningful "leave it to the
    // game"; this one does not, because unset already means on - a third label
    // saying "Game default" next to a value of On would be a distinction with
    // no difference and one more press to get past.
    settings_.performance = (settings_.performance == 0) ? 1 : 0;
  } else {
    return false;  // not a settings control; let the caller handle it
  }

  PersistSettings();
  RenderSettingsPanel();
  return true;
}

void LauncherApp::PersistSettings() {
  if (SaveSettings(SettingsFile(), settings_)) return;
  // Not fatal and not a state change: the settings still apply to this session,
  // and taking the user off the screen they are editing to report a disk
  // problem would lose the edit they just made.
  std::fprintf(stderr, "thps_p8_gui: could not write %s\n", SettingsFile().string().c_str());
  SetText("settings_note", "These could not be saved, so they will apply to this session only.");
}

// -------------------------------------------------------------- extraction ---

void LauncherApp::BeginExtract() {
  SetState(State::kExtracting);
  extract_progress_.store(0.0);
  extract_done_.store(0);
  extract_total_.store(disc_bytes_);
  extract_result_.store(0);
  extract_cancelled_.store(false);

  const std::string tool = platform::SiblingExe(portable_dir_, "thps_p8_identify").string();
  const std::string game_dir = (portable_dir_ / "game").string();
  const std::string image = disc_path_;

  // The worker only writes atomics. Every RmlUi call stays on the loop.
  extract_thread_ = std::thread([this, tool, game_dir, image]() {
    platform::Child child = platform::Start(
        {tool, "--identify_disc=" + image, "--extract_to=" + game_dir, "--json"});
    if (!child.valid()) {
      extract_result_.store(2);
      return;
    }
    {
      // Published before the first read, so a Cancel arriving in the same frame
      // as the start still finds something to stop.
      std::lock_guard<std::mutex> lock(extract_mutex_);
      extract_child_ = std::make_unique<platform::Child>(std::move(child));
    }

    bool exact = false;
    platform::Child* running = nullptr;
    {
      std::lock_guard<std::mutex> lock(extract_mutex_);
      running = extract_child_.get();
    }
    const int rc = running->Drain([this, &exact](const std::string& line) {
      double fraction = 0.0;
      unsigned long long done = 0, total = 0;
      if (std::sscanf(line.c_str(), "PROGRESS %lf %llu %llu", &fraction, &done, &total) == 3) {
        extract_progress_.store(fraction);
        extract_done_.store(done);
        if (total) extract_total_.store(total);
      } else if (line.find("\"EXACT\"") != std::string::npos) {
        exact = true;
      }
    });

    {
      std::lock_guard<std::mutex> lock(extract_mutex_);
      extract_child_.reset();
    }

    if (extract_cancelled_.load()) {
      // Remove the partial. install.toml is written last, so the install
      // already reads as "not done" and first run would simply repeat - but the
      // orphaned bytes were never reclaimed, and 4.7 GB of them is not
      // something to leave lying in a folder the user was told is portable.
      std::error_code ec;
      std::filesystem::remove_all(game_dir, ec);
      if (ec) {
        std::fprintf(stderr, "thps_p8_gui: could not remove partial install at %s: %s\n",
                     game_dir.c_str(), ec.message().c_str());
      }
      extract_result_.store(3);
      return;
    }
    extract_result_.store((rc == 0 && exact) ? 1 : 2);
  });
}

void LauncherApp::CancelExtract() {
  if (state_ != State::kExtracting && !extract_thread_.joinable()) return;
  extract_cancelled_.store(true);
  std::lock_guard<std::mutex> lock(extract_mutex_);
  if (extract_child_) extract_child_->RequestStop();
}

void LauncherApp::JoinExtractThread() {
  if (extract_thread_.joinable()) extract_thread_.join();
}

void LauncherApp::FinishInstall() {
  std::error_code ec;
  std::filesystem::create_directories(portable_dir_ / "config", ec);
  std::ofstream marker(portable_dir_ / "config" / "install.toml", std::ios::trunc);
  if (!marker) {
    SetState(State::kFailIo);
    return;
  }
  // Written last, on purpose: IsInstalled() keys off this file, so a run that
  // dies mid-extract leaves a first-run install rather than a broken "ready"
  // one. The disc path is recorded for provenance only - nothing reads it back
  // to find game data, because the game data is inside this directory.
  marker << "# Written by thps_p8_gui on first run. Delete to set up again.\n"
         << "[install]\n"
         << "game_data = \"game\"\n"
         << "source_image = \"" << disc_path_ << "\"\n";
  marker.close();
  if (!marker) {
    SetState(State::kFailIo);
    return;
  }
  SetState(State::kDone);
}

std::vector<std::string> LauncherApp::LaunchArgv() const {
  if (!should_launch_) return {};
  std::error_code ec;
  std::filesystem::create_directories(portable_dir_ / "logs", ec);
  LaunchPaths paths;
  paths.launcher_exe = platform::SiblingExe(portable_dir_, "thps_p8_launch");
  paths.game_data_root = portable_dir_ / "game";
  paths.breadcrumb = portable_dir_ / "logs" / "last_launch.json";
  // The required flags and the omit-empty rule both live in RenderArgv, so
  // this function has no opinions left to hold. See settings.h.
  return RenderArgv(paths, settings_);
}

CheckResult LauncherApp::RunCheck() const {
  CheckResult out;
  const std::filesystem::path launcher = platform::SiblingExe(portable_dir_, "thps_p8_launch");
  std::error_code ec;
  if (!std::filesystem::exists(launcher, ec)) return out;

  // exit status is 0/1 by design; the JSON carries the detail
  const platform::RunResult run = platform::Run({launcher.string(), "--check", "--json"});
  if (!run.ran || run.out.empty()) return out;
  const std::string& json = run.out;
  out.ran = true;

  // Deliberately not a JSON library. Four scalars and a pid list, from a
  // producer in this same repo whose shape is pinned by schema_version - a
  // parser dependency would cost more than it protects.
  auto number = [&json](const char* key, long long fallback) -> long long {
    const size_t at = json.find(key);
    if (at == std::string::npos) return fallback;
    const size_t colon = json.find(':', at + std::strlen(key));
    if (colon == std::string::npos) return fallback;
    return std::strtoll(json.c_str() + colon + 1, nullptr, 10);
  };
  out.live_instances = int(number("\"live_instances\"", 0));
  out.removed = int(number("\"removed\"", 0));
  out.bytes = (unsigned long long)number("\"bytes\"", 0);
  out.sweep_ran = json.find("\"ran\": true") != std::string::npos;

  // Only the game array carries pids we may signal; stop before the launcher
  // array so a stalled launcher is never mistaken for a running game.
  const size_t games_at = json.find("\"game\": [");
  const size_t launchers_at = json.find("\"launcher\": [");
  size_t cursor = games_at;
  while (cursor != std::string::npos) {
    cursor = json.find("\"pid\":", cursor);
    if (cursor == std::string::npos) break;
    if (launchers_at != std::string::npos && cursor > launchers_at) break;
    out.game_pids.push_back(int(std::strtol(json.c_str() + cursor + 6, nullptr, 10)));
    cursor += 6;
  }
  return out;
}

void LauncherApp::RefreshHomeState() {
  ShowScreen(Screen::kHome);
  last_check_ = RunCheck();
  // A live instance outranks
  // debris, because starting a second copy is the failure that black-screens
  // both of them.
  if (last_check_.live_instances > 0) {
    SetState(State::kBlocked);
  } else if (last_check_.sweep_ran && last_check_.removed > 0) {
    SetState(State::kDebris);
  } else {
    SetState(State::kReady);
    // Only worth mentioning when nothing else is wrong: a past failure is
    // context, not a blocker, and it must not out-shout a live problem.
    if (!last_run_note_.empty()) SetText("detail", last_run_note_);
  }
}

void LauncherApp::ReadLastRun() {
  last_run_note_.clear();
  const std::filesystem::path crumb = portable_dir_ / "logs" / "last_launch.json";
  std::ifstream in(crumb);
  if (!in) return;  // no previous run, which is the common case
  const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  const size_t at = json.find("\"exit_status\"");
  if (at == std::string::npos) return;
  const size_t colon = json.find(':', at);
  if (colon == std::string::npos) return;
  const long status = std::strtol(json.c_str() + colon + 1, nullptr, 10);
  if (status == 0) return;  // it ended fine; say nothing

  // A failure is context on an otherwise-ready screen, not an error state, and
  // it must not use exit-code vocabulary. "Signalled" is the only distinction a
  // player can act on differently, and even that only softens the wording.
  const bool signalled = json.find("\"signalled\": true") != std::string::npos;
  last_run_note_ = signalled
                       ? "Your last session stopped unexpectedly. You can play again from here."
                       : "Your last session did not finish cleanly. You can play again from here.";
}

void LauncherApp::OpenPortableFolder(const char* folder) const {
  std::error_code ec;
  const std::filesystem::path path = portable_dir_ / folder;
  std::filesystem::create_directories(path, ec);
  if (!platform::OpenFolder(path)) {
    std::fprintf(stderr, "thps_p8_gui: could not open %s\n", path.string().c_str());
  }
}

}  // namespace thps
