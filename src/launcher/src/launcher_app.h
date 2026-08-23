// The launcher's state, and the only thing that knows which screen is showing.
//
// Two documents, never both visible: first-run and home. Within each, the
// visible panel is chosen by a single class on <body> (`st-welcome`,
// `st-verifying`, ...) and RCSS does the rest. Keeping the state in one enum
// and the gating in one stylesheet rule is what stops "which panel is showing"
// from becoming a property of five different elements that can disagree.
//
// Nothing in this file touches the OS directly. Running a child, stopping one,
// asking about free space and opening a folder all go through platform.h, which
// is what lets the Windows build be a second .inc rather than a second UI.

#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <RmlUi/Core.h>

#include "platform.h"
#include "settings.h"

namespace thps {

enum class Screen {
  kFirstRun,
  kHome,
};

enum class State {
  // First-run
  kWelcome,        // no install yet; invite the user to pick their disc
  kPicking,        // the native file dialog is open
  kVerifying,      // reading identity out of the chosen image
  kExtracting,     // copying the game data into the portable directory
  kDone,           // install.toml written; offer to continue
  kFailNotDisc,    // the file is not an Xbox 360 disc
  kFailWrongBuild, // right game, different release than this port was built for
  kFailNotGame,    // a disc, but not this game
  kFailIo,         // could not read the image, or could not write the install
  kFailNoSpace,    // not enough room, said BEFORE anything is copied
  kCancelled,      // the user stopped the copy; the partial has been removed
  // Home
  kReady,          // installed and verified; Play is live
  kStarting,       // handing off to the launcher
  kDebris,         // leftover memory from a crash; reclaimable, Play is not live
  kBlocked,        // the game is already running; Play is not live
  kSettings,       // the display settings screen
  kAbout,          // licences and notices
};

// What `thps_p8_launch --check --json` said. The GUI forms no opinion of its
// own about process state - it renders this.
struct CheckResult {
  bool ran = false;              // the command itself worked
  int live_instances = 0;
  bool sweep_ran = false;
  int removed = 0;
  unsigned long long bytes = 0;
  std::vector<int> game_pids;    // so "Stop" has something to signal
};

// Set by the SDL file-dialog callback, which SDL may invoke on another thread.
// The loop drains it; nothing else touches RmlUi off the main thread.
class PendingPick {
 public:
  void Set(std::string path, bool cancelled);
  // Returns true if a pick was waiting, and clears it.
  bool Take(std::string* out_path, bool* out_cancelled);

 private:
  std::mutex mutex_;
  bool has_ = false;
  bool cancelled_ = false;
  std::string path_;
};

class LauncherApp {
 public:
  LauncherApp(Rml::Context* context, std::filesystem::path ui_dir,
              std::filesystem::path portable_dir);
  ~LauncherApp();

  // Loads both documents and shows whichever the install state calls for.
  bool Initialise();

  void SetState(State state);
  State state() const { return state_; }

  // Called from the loop once per frame; drains the file-dialog handoff and
  // moves the extraction bar.
  void Poll();

  // Element event entry point. Buttons carry an `id` and this maps it to work.
  void OnAction(const Rml::String& id);

  bool quit_requested() const { return quit_requested_; }

  // True when the loop should exit *in order to* hand off to the launcher.
  bool should_launch() const { return should_launch_; }
  // argv for that handoff, already absolute. Empty if should_launch() is false.
  std::vector<std::string> LaunchArgv() const;

  // True when the portable directory already holds a finished install.
  bool IsInstalled() const;

 private:
  void ShowScreen(Screen screen);
  void ApplyStateClass();
  void SetText(const char* element_id, const std::string& text);
  void BeginPick();
  void VerifyPicked(const std::string& path);
  // Looks for a disc image sitting beside the launcher and verifies it. Called
  // once, from Initialise(), and only when nothing is installed yet.
  void ScanForNearbyDisc();
  // Recognises a game directory that is already extracted but carries no
  // install marker, and writes the marker rather than asking for the disc again.
  void AdoptExistingInstall();
  // First-run only: picks display defaults that suit a handheld panel.
  void SeedHandheldDefaults();
  void ShowNearbyDisc();
  // Returns false and moves to kFailNoSpace when the volume cannot hold the
  // install. `needed` is what the disc image reported, not an estimate.
  bool CheckSpaceFor(std::uint64_t needed);
  void BeginExtract();
  void CancelExtract();
  void JoinExtractThread();
  void FinishInstall();
  CheckResult RunCheck() const;
  // Re-runs --check and moves home to ready / debris / blocked accordingly.
  void RefreshHomeState();
  void OpenSaveFolder() const;
  // Reads the breadcrumb the previous run left, if any.
  void ReadLastRun();

  // --- settings -------------------------------------------------------------
  std::filesystem::path SettingsFile() const;
  void EnumerateDisplays();
  void ShowSettings();
  void RenderSettingsPanel();
  // `id` is one of the settings control ids; returns true if it was one.
  bool HandleSettingsAction(const Rml::String& id);
  void PersistSettings();

  CheckResult last_check_;
  std::string last_run_note_;

  // Identity is 58 ms and runs inline. Extraction copies ~4.7 GB and cannot, so
  // it runs on a worker. The worker writes only these atomics; the loop reads
  // them and owns every RmlUi call.
  //
  // `extract_child_` is what makes Cancel possible at all: it is the running
  // process, held so the UI thread can stop it while the worker is blocked
  // reading its output. Guarded because both threads touch it.
  std::thread extract_thread_;
  std::mutex extract_mutex_;
  std::unique_ptr<platform::Child> extract_child_;
  std::atomic<double> extract_progress_{0.0};
  std::atomic<std::uint64_t> extract_done_{0};
  std::atomic<std::uint64_t> extract_total_{0};
  std::atomic<int> extract_result_{0};  // 0 running, 1 succeeded, 2 failed, 3 cancelled
  std::atomic<bool> extract_cancelled_{false};

  Rml::Context* context_ = nullptr;
  std::filesystem::path ui_dir_;
  std::filesystem::path portable_dir_;

  Rml::ElementDocument* first_run_ = nullptr;
  Rml::ElementDocument* home_ = nullptr;
  Rml::ElementDocument* active_ = nullptr;

  Screen screen_ = Screen::kFirstRun;
  State state_ = State::kWelcome;
  // Where "Back" returns to from settings/about. Both are reached from ready
  // and both return there, but naming it keeps the two screens from having to
  // know that.
  State return_state_ = State::kReady;
  PendingPick pending_pick_;
  bool quit_requested_ = false;
  bool should_launch_ = false;
  std::string disc_path_;
  std::uint64_t disc_bytes_ = 0;  // as reported by identity, before any copy
  // A verified disc image found next to the launcher, if there was one.
  std::string nearby_disc_;

  Settings settings_;
  std::vector<DisplayOption> displays_;
};

}  // namespace thps
